#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "narf_conf.h"
#include "narf.h"
#include "narf_io.h"

#ifdef __GNUC__
   #define PACKED __attribute__((packed))
#else
   #define PACKED
#endif

#define SIGNATURE 0x4652414E // little endian 'NARF'
#define VERSION 0x00000007
#define END INVALID_NAF
#define NARF_MIN_FS_SECTORS 4
#define NARF_MAX_AVL_DEPTH 96
#define TRASH_MAX (NARF_MAX_AVL_DEPTH * 4)
#define SPARE_MAX 64

// Uncomment for unicode line drawing characters in debug functions
#define USE_UTF8_LINE_DRAWING

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
   #define static_assert(x,y) _Static_assert(x,y)
#else
   #define static_assert(x,y)
#endif

#ifdef NARF_MBR_UTILS
#include "narf_mbr.h"
#endif

typedef struct PACKED {
   NarfSector   m_start;
   NarfSector   m_length;
   NarfByteSize m_bytes;
   uint8_t      m_metadata[NARF_METADATA_SIZE];
} DataPayload;

typedef struct PACKED {
   NarfSector m_start;
   NarfSector m_length;
} FreePayload;

#define INIT_DEFRAG_SEARCH ((FreePayload){ END, 0 })

#define ROOT_FIELDS                        \
   union {                                 \
      uint32_t m_signature;                \
      uint8_t  m_sigbytes[4];              \
   };                                      \
   uint32_t     m_narf_version;            \
   NarfByteSize m_sector_size;             \
   NarfSector   m_total_sectors;           \
   NarfSector      m_data_root;               \
   NarfSector      m_free_root;               \
                                           \
   NarfSector   m_spare_count;             \
   NarfSector   m_spare_inline[SPARE_MAX]; \
                                           \
   NarfSector   m_count;                   \
   NarfSector   m_bottom;                  \
   NarfSector   m_top;                     \
   NarfSector   m_origin;                  \
   uint32_t     m_root_version;            \
   uint32_t     m_lfsr_seed;

typedef struct {
   ROOT_FIELDS
} RootState;

// Spare catalog-node sectors.
//
// NARF stores AVL/catalog nodes in ordinary sectors near the high end
// of the volume. Because catalog updates are copy-on-write, old node
// sectors cannot be reused until after the new root has committed.
//
// After commit, trash node sectors are pushed here and may be reused
// by later catalog-node allocations. These are NOT user-payload free
// sectors and are intentionally separate from the payload free tree,
// because the free tree is itself made of catalog nodes.

// Bytes occupied by Root fields other than m_reserved. Keep this next to Root.
#define ROOT_PREFIX_BYTES (sizeof(RootState))
#define ROOT_RESERVED_BYTES (NARF_SECTOR_SIZE - ROOT_PREFIX_BYTES - sizeof(uint32_t))

typedef struct PACKED {
   ROOT_FIELDS
   uint8_t      m_reserved[ROOT_RESERVED_BYTES];
   uint32_t     m_checksum;
} Root;
static_assert(sizeof(Root) == NARF_SECTOR_SIZE, "Root wrong size");

#define NODE_FIELDS             \
   NarfSector      m_left;         \
   NarfSector      m_right;        \
   uint8_t      m_height;       \
   union {                      \
      DataPayload m_data;       \
      FreePayload m_free;       \
   };                           \
   uint32_t     m_root_version; \
   uint32_t     m_node_version;

typedef struct PACKED {
   NODE_FIELDS
} NodeHeader;

typedef struct PACKED {
   NODE_FIELDS
   char         m_key[NARF_SECTOR_SIZE - sizeof(NodeHeader) - sizeof(uint32_t)];
   uint32_t     m_checksum;
} Node;
static_assert(sizeof(Node) == NARF_SECTOR_SIZE, "Node wrong size");

#define BYTES2SECTORS(x) \
   (((x) / NARF_SECTOR_SIZE) + (((x) % NARF_SECTOR_SIZE) != 0))
#define KEYSIZE (sizeof(((Node *) 0)->m_key))

typedef union {
   uint8_t  bytes[NARF_SECTOR_SIZE];
   Root     root;
   Node     node;
} SectorScratch;
static_assert(sizeof(SectorScratch) == NARF_SECTOR_SIZE, "Node sector scratch size");

typedef struct {
   RootState m_root;
   uint32_t  m_lfsr_state;
} RootSnapshot;

static SectorScratch sector_work;
#define buffer        sector_work.bytes
#define root_tmp      sector_work.root
#define node_tmp      sector_work.node

static RootState root;
static RootSnapshot saved_root;
static Node node_work0;
static Node node_work1;
static int root_copy = 0;
static char dir_key[KEYSIZE];
static char key_work[KEYSIZE];
static uint32_t lfsr_state = 1;
static bool transaction_may_use_reserve = false;
static bool transaction_open = false;
static NarfSector trash_nodes[TRASH_MAX];
static unsigned trash_node_count = 0;
static bool trash_node_overflow = false;

//! @brief Compute a CRC-32 compatible with zlib/crc32().
uint32_t crc32(uint32_t crc, const void *data, size_t length) {
   const uint8_t *p = data;

   crc = ~crc;

   for (size_t i = 0; i < length; i++) {
      crc ^= p[i];
      for (int j = 0; j < 8; j++)
         crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
   }

   return ~crc;
}

//! @brief Return true when a catalog-node sector reference is null.
static bool ref_is_null(NarfSector ref) {
   return ref == END;
}

//! @brief Compare 32-bit root versions using wraparound ordering.
static bool version_after(uint32_t a, uint32_t b) {
   uint32_t diff = a - b;
   return diff != 0 && diff < 0x80000000u;
}

//! @brief Return the root version assigned to writes in the open transaction.
static uint32_t transaction_root_version(void) {
   uint32_t v = root.m_root_version + 1;

   if (v == 0) {
      v = 1;
   }

   return v;
}

//! @brief Save the current mutable root state before starting a public transaction.
static void transaction_begin(void) {
   saved_root.m_root = root;
   saved_root.m_lfsr_state = lfsr_state;
   transaction_may_use_reserve = false;
   trash_node_count = 0;
   trash_node_overflow = false;
   transaction_open = true;
}

//! @brief Restore the root state saved by transaction_begin().
static void transaction_rollback(void) {
   root = saved_root.m_root;
   lfsr_state = saved_root.m_lfsr_state;
   transaction_may_use_reserve = false;
   trash_node_count = 0;
   trash_node_overflow = false;
   transaction_open = false;
}

//! @brief Advance the persisted 32-bit LFSR and return the next token.
static uint32_t lfsr_next(void) {
   uint32_t lsb;

   if (lfsr_state == 0) {
      lfsr_state = 0x1f2e3d4c;
   }

   lsb = lfsr_state & 1;
   lfsr_state >>= 1;

   if (lsb) {
      lfsr_state ^= 0x80200003u;
   }

   if (lfsr_state == 0) {
      lfsr_state = 0x1f2e3d4c;
   }

   return lfsr_state;
}

//! @brief Build an initial nonzero LFSR seed for a new filesystem.
static uint32_t mkfs_lfsr_seed(NarfSector origin, NarfSector size) {
   uint32_t seed;

   seed = (uint32_t) rand();
   seed ^= (uint32_t) time(NULL);
   seed ^= (uint32_t) getpid();
   seed ^= (uint32_t) origin;
   seed ^= (uint32_t) size;
   seed ^= 0x1f2e3d4c;

   if (seed == 0) {
      seed = 0x1f2e3d4c;
   }

   return seed;
}

//! @brief Validate that the mounted root looks like a current NARF root.
static bool verify(void) {
   if (root.m_signature != SIGNATURE) return false;
   if (root.m_sector_size != NARF_SECTOR_SIZE) return false;
   return true;
}

//! @brief Validate a public key string.
static bool valid_key(const char *key) {
   if (key == NULL) return false;
   if (strlen(key) >= KEYSIZE) return false;
   return true;
}

//! @brief Validate a public directory-marker key string.
static bool valid_dir_key(const char *key, const char *sep) {
   size_t klen;
   size_t slen;

   if (key == NULL) return false;
   if (sep == NULL) return false;

   klen = strlen(key);
   slen = strlen(sep);

   if (klen == 0 || klen >= KEYSIZE) return false;
   if (slen == 0 || slen >= KEYSIZE) return false;
   if (klen < slen) return false;

   // Root is not represented as a stored directory key.
   if (klen == slen && strcmp(key, sep) == 0) return false;

   return strcmp(key + klen - slen, sep) == 0;
}

//! @brief Validate directory traversal arguments.
static bool valid_dir_args(const char *dirname, const char *sep) {
   if (dirname == NULL) return false;
   if (sep == NULL) return false;
   if (sep[0] == 0) return false;
   if (strlen(dirname) >= KEYSIZE) return false;
   if (strlen(sep) >= KEYSIZE) return false;
   return true;
}

//! @brief Validate a single-sector catalog-node address.
static bool valid_node_sector(NarfSector sector) {
   if (!verify()) return false;
   if (sector == END) return false;
   if (root.m_total_sectors < NARF_MIN_FS_SECTORS) return false;
   if (sector < 2) return false;
   if (sector >= root.m_total_sectors) return false;
   return true;
}

//! @brief Compute the checksum for a root block.
static uint32_t root_checksum(Root *r) {
   uint32_t old = r->m_checksum;
   uint32_t ck;
   r->m_checksum = 0;
   ck = crc32(0, r, NARF_SECTOR_SIZE - sizeof(uint32_t));
   r->m_checksum = old;
   return ck;
}

//! @brief Expand the compact in-memory root state into one on-disk sector.
static void root_to_disk(Root *out) {
   memset(out, 0, sizeof(*out));
   out->m_signature = root.m_signature;
   out->m_narf_version = root.m_narf_version;
   out->m_sector_size = root.m_sector_size;
   out->m_total_sectors = root.m_total_sectors;
   out->m_data_root = root.m_data_root;
   out->m_free_root = root.m_free_root;
   out->m_spare_count = root.m_spare_count;
   memcpy(out->m_spare_inline, root.m_spare_inline, sizeof(out->m_spare_inline));
   out->m_count = root.m_count;
   out->m_bottom = root.m_bottom;
   out->m_top = root.m_top;
   out->m_origin = root.m_origin;
   out->m_root_version = root.m_root_version;
   out->m_lfsr_seed = root.m_lfsr_seed;
}

//! @brief Load the compact in-memory root state from a validated on-disk sector.
static void root_from_disk(const Root *in) {
   root.m_signature = in->m_signature;
   root.m_narf_version = in->m_narf_version;
   root.m_sector_size = in->m_sector_size;
   root.m_total_sectors = in->m_total_sectors;
   root.m_data_root = in->m_data_root;
   root.m_free_root = in->m_free_root;
   root.m_spare_count = in->m_spare_count;
   if (root.m_spare_count > SPARE_MAX) {
      root.m_spare_count = 0;
   }
   memcpy(root.m_spare_inline, in->m_spare_inline, sizeof(root.m_spare_inline));
   root.m_count = in->m_count;
   root.m_bottom = in->m_bottom;
   root.m_top = in->m_top;
   root.m_origin = in->m_origin;
   root.m_root_version = in->m_root_version;
   root.m_lfsr_seed = in->m_lfsr_seed;
}

//! @brief Read and validate one of the two root copies.
static bool read_root_copy(NarfSector origin, int which, Root *out) {
   if (!narf_io_read(origin + (NarfSector) which, out)) return false;
   if (out->m_signature != SIGNATURE) return false;
   if (out->m_narf_version != VERSION) return false;
   if (out->m_sector_size != NARF_SECTOR_SIZE) return false;
   if (out->m_checksum != root_checksum(out)) return false;
   return true;
}

//! @brief Read and validate a root copy, returning only its root version.
static bool read_root_copy_version(NarfSector origin, int which, uint32_t *version) {
   if (version == NULL) return false;
   if (!read_root_copy(origin, which, &root_tmp)) return false;
   *version = root_tmp.m_root_version;
   return true;
}

//! @brief Commit the current in-memory root as the newest root copy.
static bool commit_root(void) {
   int dest = 1 - root_copy;
   root.m_root_version = transaction_root_version();
   root.m_lfsr_seed = lfsr_next();
   root_to_disk(&root_tmp);
   root_tmp.m_checksum = 0;
   root_tmp.m_checksum = crc32(0, &root_tmp, NARF_SECTOR_SIZE - sizeof(uint32_t));
   if (!narf_io_write(root.m_origin + (NarfSector) dest, &root_tmp)) return false;
   root_copy = dest;
   transaction_may_use_reserve = false;
   transaction_open = false;
   return true;
}

//! @brief Initialize an empty root block for a new filesystem.
static bool init_root(NarfSector origin, NarfSector size) {
   memset(&root, 0, sizeof(root));
   root.m_signature = SIGNATURE;
   root.m_narf_version = VERSION;
   root.m_sector_size = NARF_SECTOR_SIZE;
   root.m_total_sectors = size;
   root.m_data_root = END;
   root.m_free_root = END;
   root.m_spare_count = 0;
   for (unsigned i = 0; i < SPARE_MAX; i++) {
      root.m_spare_inline[i] = END;
   }
   root.m_count = 0;
   root.m_bottom = 2;
   root.m_top = size;
   root.m_origin = origin;
   root.m_root_version = 0;
   root.m_lfsr_seed = mkfs_lfsr_seed(origin, size);
   lfsr_state = root.m_lfsr_seed;

   root_to_disk(&root_tmp);
   root_tmp.m_checksum = 0;
   root_tmp.m_checksum = crc32(0, &root_tmp, NARF_SECTOR_SIZE - sizeof(uint32_t));
   if (!narf_io_write(origin + 1, &root_tmp)) return false;

   root.m_root_version = 1;
   root_to_disk(&root_tmp);
   root_tmp.m_checksum = 0;
   root_tmp.m_checksum = crc32(0, &root_tmp, NARF_SECTOR_SIZE - sizeof(uint32_t));
   if (!narf_io_write(origin + 0, &root_tmp)) return false;

   root_copy = 0;
   return true;
}

//! @brief Compute the checksum for a catalog node.
static uint32_t node_checksum(Node *n) {
   uint32_t old = n->m_checksum;
   uint32_t ck;
   n->m_checksum = 0;
   ck = crc32(0, n, NARF_SECTOR_SIZE - sizeof(uint32_t));
   n->m_checksum = old;
   return ck;
}

//! @brief Mark a committed catalog-node sector as trash until commit succeeds.
static void trash_node(NarfSector ref) {
   if (!transaction_open) return;
   if (ref_is_null(ref)) return;
   if (!valid_node_sector(ref)) return;

   for (unsigned i = 0; i < trash_node_count; i++) {
      if (trash_nodes[i] == ref) return;
   }

   if (trash_node_count < TRASH_MAX) {
      trash_nodes[trash_node_count++] = ref;
   }
   else {
      trash_node_overflow = true;
   }
}

//! @brief Pop one sector from the rollback-safe root-resident spare stack.
static bool pop_spare(NarfSector *ref) {
   if (ref == NULL) return false;

   while (root.m_spare_count > 0) {
      NarfSector sector = root.m_spare_inline[--root.m_spare_count];
      root.m_spare_inline[root.m_spare_count] = END;

      if (!valid_node_sector(sector)) {
         continue;
      }

      *ref = sector;
      return true;
   }

   return false;
}

//! @brief Add one committed-safe catalog-node sector to the spare stack.
static bool push_spare(NarfSector sector) {
   if (!valid_node_sector(sector)) return true;

   for (NarfSector i = 0; i < root.m_spare_count && i < SPARE_MAX; i++) {
      if (root.m_spare_inline[i] == sector) {
         return true;
      }
   }

   if (root.m_spare_count >= SPARE_MAX) {
      return true;
   }

   root.m_spare_inline[root.m_spare_count++] = sector;
   return true;
}

static bool alloc_node_sector(NarfSector *ref);

//! @brief Read a raw single-sector node when its checksum is valid.
static bool read_node_any(NarfSector sector, Node *out) {
   if (out == NULL) return false;
   if (!valid_node_sector(sector)) return false;
   if (!narf_io_read(root.m_origin + sector, out)) return false;
   if (out->m_checksum != node_checksum(out)) return false;
   if (out->m_node_version == 0) return false;
   return true;
}

//! @brief Read a catalog node by sector reference.
static bool read_node(NarfSector ref, Node *out) {
   if (ref_is_null(ref)) return false;
   if (!valid_node_sector(ref)) return false;
   if (!read_node_any(ref, out)) return false;
   return true;
}

//! @brief Choose a nonzero node version that differs from visible contents.
static uint32_t new_node_version(uint32_t old, NarfSector sector) {
   uint32_t v;
   uint32_t current = 0;

   if (valid_node_sector(sector) && read_node_any(sector, &node_tmp)) {
      current = node_tmp.m_node_version;
   }

   do {
      v = lfsr_next();
   } while (v == 0 || v == old || v == current);

   return v;
}

//! @brief Write a catalog node, reusing transaction-private sectors or COWing committed ones.
//!
//! If oldref names a node already written by this transaction, the same sector
//! is rewritten.  Otherwise a fresh/spare sector is allocated and the old
//! committed sector is marked as trash for post-commit recycling.
static bool write_node(NarfSector oldref, Node *n, NarfSector *newref) {
   uint32_t txver;
   uint32_t oldver = 0;
   NarfSector ref = END;

   if (n == NULL || newref == NULL) return false;

   txver = transaction_root_version();

   if (!ref_is_null(oldref) && read_node(oldref, &node_tmp)) {
      oldver = node_tmp.m_node_version;

      if (node_tmp.m_root_version == txver) {
         ref = oldref;
         n->m_node_version = oldver;
      }
   }

   if (ref_is_null(ref)) {
      if (!alloc_node_sector(&ref)) return false;
      n->m_node_version = new_node_version(oldver, ref);
      trash_node(oldref);
   }

   n->m_root_version = txver;
   n->m_checksum = 0;
   n->m_checksum = crc32(0, n, NARF_SECTOR_SIZE - sizeof(uint32_t));

   if (!narf_io_write(root.m_origin + ref, n)) {
      return false;
   }

   *newref = ref;
   return true;
}

//! @brief Return the AVL height for a referenced node.
static int height(NarfSector ref) {
   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &node_tmp)) return 0;
   return node_tmp.m_height;
}

//! @brief Recompute an AVL node height from its children.
static void update_height(Node *n) {
   int lh = height(n->m_left);
   int rh = height(n->m_right);
   n->m_height = (uint8_t)((lh > rh ? lh : rh) + 1);
}

//! @brief Compute the AVL balance factor for a referenced node.
static int balance_factor(NarfSector ref) {
   NarfSector left;
   NarfSector right;

   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &node_tmp)) return 0;
   left = node_tmp.m_left;
   right = node_tmp.m_right;
   return height(left) - height(right);
}

//! @brief Perform a copy-on-write AVL right rotation.
static bool rotate_right(NarfSector yref, NarfSector *out) {
   NarfSector xref;
   NarfSector y2;
   NarfSector x2;

   if (!read_node(yref, &node_work0)) return false;
   xref = node_work0.m_left;
   if (!read_node(xref, &node_work1)) return false;

   node_work0.m_left = node_work1.m_right;
   update_height(&node_work0);
   if (!write_node(yref, &node_work0, &y2)) return false;

   node_work1.m_right = y2;
   update_height(&node_work1);
   if (!write_node(xref, &node_work1, &x2)) return false;

   *out = x2;
   return true;
}

//! @brief Perform a copy-on-write AVL left rotation.
static bool rotate_left(NarfSector xref, NarfSector *out) {
   NarfSector yref;
   NarfSector x2;
   NarfSector y2;

   if (!read_node(xref, &node_work0)) return false;
   yref = node_work0.m_right;
   if (!read_node(yref, &node_work1)) return false;

   node_work0.m_right = node_work1.m_left;
   update_height(&node_work0);
   if (!write_node(xref, &node_work0, &x2)) return false;

   node_work1.m_left = x2;
   update_height(&node_work1);
   if (!write_node(yref, &node_work1, &y2)) return false;

   *out = y2;
   return true;
}

//! @brief Rebalance a copy-on-write AVL subtree.
static bool rebalance(NarfSector ref, NarfSector *out) {
   NarfSector child;
   NarfSector tmp;
   int bf;

   if (ref_is_null(ref)) {
      *out = ref;
      return true;
   }

   bf = balance_factor(ref);

   if (bf > 1) {
      if (!read_node(ref, &node_work0)) return false;
      child = node_work0.m_left;
      if (balance_factor(child) < 0) {
         if (!rotate_left(child, &tmp)) return false;
         if (!read_node(ref, &node_work0)) return false;
         node_work0.m_left = tmp;
         update_height(&node_work0);
         if (!write_node(ref, &node_work0, &ref)) return false;
      }
      return rotate_right(ref, out);
   }

   if (bf < -1) {
      if (!read_node(ref, &node_work0)) return false;
      child = node_work0.m_right;
      if (balance_factor(child) > 0) {
         if (!rotate_right(child, &tmp)) return false;
         if (!read_node(ref, &node_work0)) return false;
         node_work0.m_right = tmp;
         update_height(&node_work0);
         if (!write_node(ref, &node_work0, &ref)) return false;
      }
      return rotate_left(ref, out);
   }

   if (!read_node(ref, &node_work0)) return false;
   update_height(&node_work0);
   return write_node(ref, &node_work0, out);
}

//! @brief Compare a free-tree search key with a free-tree node.
static int free_cmp_values(NarfSector length, NarfSector start, NarfSector sector, const Node *n, NarfSector nsector) {
   if (length < n->m_free.m_length) return -1;
   if (length > n->m_free.m_length) return 1;
   if (start < n->m_free.m_start) return -1;
   if (start > n->m_free.m_start) return 1;
   if (sector < nsector) return -1;
   if (sector > nsector) return 1;
   return 0;
}

//! @brief Find a data-tree node by key.
static bool data_find_ref_rec(NarfSector ref, const char *key, NarfSector *found, Node *outnode) {
   int cmp;

   while (!ref_is_null(ref)) {
      if (!read_node(ref, &node_work0)) return false;
      cmp = strcmp(key, node_work0.m_key);
      if (cmp == 0) {
         if (found) *found = ref;
         if (outnode) *outnode = node_work0;
         return true;
      }
      ref = (cmp < 0) ? node_work0.m_left : node_work0.m_right;
   }

   return false;
}

//! @brief Insert a node reference into the data AVL tree.
static bool data_insert_rec(NarfSector rootref, NarfSector itemref, const char *key, NarfSector *out) {
   int cmp;
   NarfSector next;
   NarfSector child;

   if (ref_is_null(rootref)) {
      *out = itemref;
      return true;
   }

   if (!read_node(rootref, &node_work0)) return false;
   cmp = strcmp(key, node_work0.m_key);
   if (cmp == 0) return false;
   next = (cmp < 0) ? node_work0.m_left : node_work0.m_right;

   if (!data_insert_rec(next, itemref, key, &child)) return false;
   if (!read_node(rootref, &node_work0)) return false;

   if (cmp < 0) {
      node_work0.m_left = child;
   }
   else {
      node_work0.m_right = child;
   }

   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Detach and return the smallest node; caller owns the returned ref.
static bool delete_min_rec(NarfSector rootref, NarfSector *out, NarfSector *minref) {
   NarfSector left;
   NarfSector right;
   NarfSector child;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;

   if (ref_is_null(left)) {
      // Return the detached node still live.  The caller may reuse it as a
      // successor, or trash it later if write_node() COWs it elsewhere.
      if (minref) *minref = rootref;
      *out = right;
      return true;
   }

   if (!delete_min_rec(left, &child, minref)) return false;
   if (!read_node(rootref, &node_work0)) return false;
   node_work0.m_left = child;
   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Delete a key from the data AVL tree.
static bool data_delete_rec(NarfSector rootref, const char *key, NarfSector *out, NarfSector *removed_ref, DataPayload *removed_data) {
   NarfSector left;
   NarfSector right;
   NarfSector next;
   NarfSector child;
   NarfSector succref;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0)) return false;

   cmp = strcmp(key, node_work0.m_key);
   left = node_work0.m_left;
   right = node_work0.m_right;

   if (cmp < 0) {
      next = left;
      if (!data_delete_rec(next, key, &child, removed_ref, removed_data)) return false;
      if (!read_node(rootref, &node_work0)) return false;
      node_work0.m_left = child;
   }
   else if (cmp > 0) {
      next = right;
      if (!data_delete_rec(next, key, &child, removed_ref, removed_data)) return false;
      if (!read_node(rootref, &node_work0)) return false;
      node_work0.m_right = child;
   }
   else {
      *removed_ref = rootref;
      trash_node(rootref);
      if (removed_data) *removed_data = node_work0.m_data;
      if (ref_is_null(left)) {
         *out = right;
         return true;
      }
      if (ref_is_null(right)) {
         *out = left;
         return true;
      }
      if (!delete_min_rec(right, &child, &succref)) return false;
      if (!read_node(rootref, &node_work0)) return false;
      *removed_ref = rootref;
      if (removed_data) *removed_data = node_work0.m_data;
      if (!read_node(succref, &node_work1)) return false;
      node_work1.m_left = left;
      node_work1.m_right = child;
      update_height(&node_work1);
      if (!write_node(succref, &node_work1, &rootref)) return false;
      // If write_node() COWed the successor, its old sector is no longer live.
      // If it rewrote in place, trashing it would corrupt the final tree.
      if (rootref != succref) trash_node(succref);
      return rebalance(rootref, out);
   }

   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Replace the data payload for an existing data-tree key.
static bool data_update_rec(NarfSector rootref, const char *key, const Node *newnode, NarfSector *out) {
   NarfSector next;
   NarfSector child;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0)) return false;

   cmp = strcmp(key, node_work0.m_key);
   if (cmp < 0) {
      next = node_work0.m_left;
      if (!data_update_rec(next, key, newnode, &child)) return false;
      if (!read_node(rootref, &node_work0)) return false;
      node_work0.m_left = child;
   }
   else if (cmp > 0) {
      next = node_work0.m_right;
      if (!data_update_rec(next, key, newnode, &child)) return false;
      if (!read_node(rootref, &node_work0)) return false;
      node_work0.m_right = child;
   }
   else {
      node_work0 = *newnode;
   }

   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Insert an extent node into the free AVL tree.
static bool free_insert_rec(NarfSector rootref, NarfSector itemref, NarfSector length, NarfSector start, NarfSector *out) {
   NarfSector next;
   NarfSector child;
   int cmp;

   if (ref_is_null(rootref)) {
      *out = itemref;
      return true;
   }

   if (!read_node(rootref, &node_work0)) return false;
   cmp = free_cmp_values(length, start, itemref, &node_work0, rootref);
   if (cmp == 0) return false;
   next = (cmp < 0) ? node_work0.m_left : node_work0.m_right;

   if (!free_insert_rec(next, itemref, length, start, &child)) return false;
   if (!read_node(rootref, &node_work0)) return false;

   if (cmp < 0) {
      node_work0.m_left = child;
   }
   else {
      node_work0.m_right = child;
   }

   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Delete a specific extent node from the free AVL tree.
static bool free_delete_rec(NarfSector rootref, NarfSector length, NarfSector start, NarfSector sector, NarfSector *out, NarfSector *removed_ref, FreePayload *removed_free) {
   NarfSector left;
   NarfSector right;
   NarfSector next;
   NarfSector child;
   NarfSector succref;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0)) return false;

   cmp = free_cmp_values(length, start, sector, &node_work0, rootref);
   left = node_work0.m_left;
   right = node_work0.m_right;

   if (cmp < 0) {
      next = left;
      if (!free_delete_rec(next, length, start, sector, &child, removed_ref, removed_free)) return false;
      if (!read_node(rootref, &node_work0)) return false;
      node_work0.m_left = child;
   }
   else if (cmp > 0) {
      next = right;
      if (!free_delete_rec(next, length, start, sector, &child, removed_ref, removed_free)) return false;
      if (!read_node(rootref, &node_work0)) return false;
      node_work0.m_right = child;
   }
   else {
      *removed_ref = rootref;
      trash_node(rootref);
      if (removed_free) *removed_free = node_work0.m_free;
      if (ref_is_null(left)) {
         *out = right;
         return true;
      }
      if (ref_is_null(right)) {
         *out = left;
         return true;
      }
      if (!delete_min_rec(right, &child, &succref)) return false;
      if (!read_node(rootref, &node_work0)) return false;
      *removed_ref = rootref;
      if (removed_free) *removed_free = node_work0.m_free;
      if (!read_node(succref, &node_work1)) return false;
      node_work1.m_left = left;
      node_work1.m_right = child;
      update_height(&node_work1);
      if (!write_node(succref, &node_work1, &rootref)) return false;
      // If write_node() COWed the successor, its old sector is no longer live.
      // If it rewrote in place, trashing it would corrupt the final tree.
      if (rootref != succref) trash_node(succref);
      return rebalance(rootref, out);
   }

   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}


//! @brief Find a free-tree node whose payload starts at a specific sector.
static bool free_find_start_rec(NarfSector ref, NarfSector start, NarfSector *found, FreePayload *outfree) {
   NarfSector left;
   NarfSector right;
   FreePayload fp;

   if (ref_is_null(ref)) return false;
   if (!read_node(ref, &node_work0)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;
   fp = node_work0.m_free;

   if (fp.m_start == start) {
      if (found) *found = ref;
      if (outfree) *outfree = fp;
      return true;
   }

   if (free_find_start_rec(left, start, found, outfree)) return true;
   return free_find_start_rec(right, start, found, outfree);
}

//! @brief Find a free-tree node whose payload ends exactly at a specific sector.
static bool free_find_end_rec(NarfSector ref, NarfSector end, NarfSector *found, FreePayload *outfree) {
   NarfSector left;
   NarfSector right;
   FreePayload fp;

   if (ref_is_null(ref)) return false;
   if (!read_node(ref, &node_work0)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;
   fp = node_work0.m_free;

   if (fp.m_start != END && fp.m_length != 0 &&
       fp.m_length <= ((NarfSector) -1) - fp.m_start &&
       fp.m_start + fp.m_length == end) {
      if (found) *found = ref;
      if (outfree) *outfree = fp;
      return true;
   }

   if (free_find_end_rec(left, end, found, outfree)) return true;
   return free_find_end_rec(right, end, found, outfree);
}

//! @brief Find the smallest free extent that can satisfy an allocation.
static bool free_best_rec(NarfSector ref, NarfSector need, NarfSector *bestref, Node *bestnode) {
   bool found = false;

   while (!ref_is_null(ref)) {
      if (!read_node(ref, &node_work0)) return false;
      if (node_work0.m_free.m_length >= need) {
         *bestref = ref;
         *bestnode = node_work0;
         found = true;
         ref = node_work0.m_left;
      }
      else {
         ref = node_work0.m_right;
      }
   }

   return found;
}

//! @brief Sum positive-length free extents in the free tree.
static bool free_sector_count_rec(NarfSector ref, NarfSector *sectors) {
   NarfSector left;
   NarfSector right;
   NarfSector free_start;
   NarfSector free_length;

   if (sectors == NULL) return false;
   if (ref_is_null(ref)) return true;
   if (!read_node(ref, &node_work0)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;
   free_start = node_work0.m_free.m_start;
   free_length = node_work0.m_free.m_length;

   if (!free_sector_count_rec(left, sectors)) return false;

   if (free_start != END && free_length != 0) {
      if (*sectors > ((NarfSector) -1) - free_length) return false;
      *sectors += free_length;
   }

   return free_sector_count_rec(right, sectors);
}

//! @brief Return the configured metadata reserve, clipped for tiny images.
static NarfSector metadata_reserve(void) {
   NarfSector r = NARF_METADATA_RESERVE_SECTORS;

   if (root.m_total_sectors < NARF_MIN_FS_SECTORS) return 0;

   // Do not let the reserve make toy/test images unusable.
   if (r > root.m_total_sectors / 4) {
      r = root.m_total_sectors / 4;
   }

   return r;
}

//! @brief Allocate a single-sector catalog node from spare or high-end space.
static bool alloc_node_sector(NarfSector *ref) {
   NarfSector reserve;

   if (ref == NULL) return false;

   if (pop_spare(ref)) {
      return true;
   }

   reserve = transaction_may_use_reserve ? 0 : metadata_reserve();
   if (root.m_top > root.m_bottom + reserve) {
      root.m_top--;
      *ref = root.m_top;
      return true;
   }

   return false;
}

//! @brief Insert an extent into the free tree, coalescing adjacent free extents.
static bool insert_free_extent_with_ref(NarfSector ref, NarfSector start, NarfSector length) {
   NarfSector seed = END;
   NarfSector written;
   NarfSector newroot;
   bool changed;

   if (length == 0) return true;
   if (start == END) return false;
   if (length > ((NarfSector) -1) - start) return false;

   if (start + length == root.m_bottom) return true;

   /*
    * The free tree is ordered by length for best-fit allocation, not by
    * address.  That makes adjacency lookup a linear tree walk, but free-space
    * insertion is already a metadata transaction and MCU RAM stays tiny.  This
    * coalescing is what keeps append-style COW writes from turning old payloads
    * into hundreds of adjacent-but-unusable fragments.
    */
   do {
      NarfSector adjref;
      NarfSector removed_ref;
      FreePayload adj;

      changed = false;

      if (length <= ((NarfSector) -1) - start &&
          free_find_start_rec(root.m_free_root, start + length, &adjref, &adj)) {
         if (!free_delete_rec(root.m_free_root, adj.m_length, adj.m_start,
                              adjref, &newroot, &removed_ref, NULL)) {
            return false;
         }
         root.m_free_root = newroot;
         if (adj.m_length > ((NarfSector) -1) - length) return false;
         length += adj.m_length;
         (void) removed_ref;
         changed = true;
      }

      if (free_find_end_rec(root.m_free_root, start, &adjref, &adj)) {
         if (!free_delete_rec(root.m_free_root, adj.m_length, adj.m_start,
                              adjref, &newroot, &removed_ref, NULL)) {
            return false;
         }
         root.m_free_root = newroot;
         if (length > ((NarfSector) -1) - adj.m_length) return false;
         start = adj.m_start;
         length += adj.m_length;
         (void) removed_ref;
         changed = true;
      }
   } while (changed);

   if (ref != END) {
      seed = ref;
   }

   memset(&node_work1, 0, sizeof(node_work1));
   node_work1.m_left = END;
   node_work1.m_right = END;
   node_work1.m_free.m_start = start;
   node_work1.m_free.m_length = length;
   node_work1.m_height = 1;
   node_work1.m_key[0] = 0;

   if (!write_node(seed, &node_work1, &written)) return false;
   if (!free_insert_rec(root.m_free_root, written, length, start, &newroot)) return false;
   root.m_free_root = newroot;
   return true;
}


//! @brief Create a free-tree node for a free data extent.
static bool insert_free_extent(NarfSector start, NarfSector length) {
   return insert_free_extent_with_ref(END, start, length);
}

//! @brief Write zeroes to every sector in an extent.
static bool zero_extent(NarfSector start, NarfSector length) {
   NarfSector i;

   if (length == 0) return true;
   if (start == END) return false;

   memset(buffer, 0, sizeof(buffer));

   for (i = 0; i < length; i++) {
      if (!narf_io_write(root.m_origin + start + i, buffer)) {
         return false;
      }
   }

   return true;
}

//! @brief Allocate data sectors for a payload extent.
static bool allocate_data_extent(NarfSector length, NarfSector *start) {
   NarfSector freeref;
   NarfSector newroot;
   NarfSector removed_ref;
   FreePayload removed_free;
   NarfSector free_start;
   NarfSector free_length;

   if (start == NULL) return false;

   if (length == 0) {
      *start = END;
      return true;
   }

   if (free_best_rec(root.m_free_root, length, &freeref, &node_work1)) {
      free_start = node_work1.m_free.m_start;
      free_length = node_work1.m_free.m_length;

      if (!free_delete_rec(root.m_free_root, free_length, free_start,
                           freeref, &newroot, &removed_ref, &removed_free)) {
         return false;
      }

      root.m_free_root = newroot;
      *start = removed_free.m_start;

      (void) removed_ref;

      if (removed_free.m_length > length) {
         return insert_free_extent(removed_free.m_start + length,
                                   removed_free.m_length - length);
      }

      return true;
   }

   if (root.m_top < root.m_bottom) return false;
   if (length > root.m_top - root.m_bottom) return false;
   if (!transaction_may_use_reserve &&
       root.m_top - root.m_bottom - length < metadata_reserve()) return false;

   *start = root.m_bottom;
   root.m_bottom += length;
   return true;
}


//! @brief Allocate sectors immediately following an existing payload extent.
static bool allocate_tail_extent(NarfSector start, NarfSector length) {
   NarfSector freeref;
   NarfSector newroot;
   NarfSector removed_ref;
   FreePayload free_node;

   if (length == 0) return true;
   if (start == END) return false;
   if (length > ((NarfSector) -1) - start) return false;

   if (start == root.m_bottom) {
      if (length > root.m_top - root.m_bottom) return false;
      if (!transaction_may_use_reserve &&
          root.m_top - root.m_bottom - length < metadata_reserve()) return false;
      root.m_bottom += length;
      return true;
   }

   if (!free_find_start_rec(root.m_free_root, start, &freeref, &free_node)) {
      return false;
   }

   if (free_node.m_length < length) return false;

   if (!free_delete_rec(root.m_free_root, free_node.m_length, free_node.m_start,
                        freeref, &newroot, &removed_ref, NULL)) {
      return false;
   }

   root.m_free_root = newroot;
   (void) removed_ref;

   if (free_node.m_length > length) {
      if (!insert_free_extent(start + length, free_node.m_length - length)) {
         return false;
      }
   }

   return true;
}

//! @brief Try the safe append-at-EOF fast path without copying old payload sectors.
static bool write_append_fast(const char *key, const uint8_t *src,
                              NarfByteSize size, NarfByteSize old_bytes,
                              NarfSector old_start, NarfSector old_length,
                              NarfByteSize new_bytes) {
   NarfSector new_length;
   NarfSector extra;
   NarfSector write_start;
   NarfSector i;
   NarfSector newroot;

   if (old_bytes % NARF_SECTOR_SIZE != 0) return false;

   new_length = BYTES2SECTORS(new_bytes);
   if (new_length < old_length) return false;
   extra = new_length - old_length;
   if (extra == 0) return false;

   if (old_length == 0) {
      if (!allocate_data_extent(new_length, &write_start)) return false;
      extra = new_length;
   }
   else {
      if (old_start == END) return false;
      if (old_length > ((NarfSector) -1) - old_start) return false;
      write_start = old_start + old_length;
      if (!allocate_tail_extent(write_start, extra)) return false;
   }

   for (i = 0; i < extra; i++) {
      NarfByteSize base;
      NarfByteSize copied = 0;

      if (i > ((NarfByteSize) -1) / NARF_SECTOR_SIZE) {
         return false;
      }

      base = ((NarfByteSize) i) * NARF_SECTOR_SIZE;
      memset(buffer, 0, sizeof(buffer));

      if (base < size) {
         copied = size - base;
         if (copied > sizeof(buffer)) copied = sizeof(buffer);
         if (src != NULL) {
            memcpy(buffer, src + base, copied);
         }
      }

      if (src == NULL && copied != 0) {
         memset(buffer, 0, copied);
      }

      if (!narf_io_write(root.m_origin + write_start + i, buffer)) {
         return false;
      }
   }

   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) {
      return false;
   }

   if (old_length == 0) {
      node_work1.m_data.m_start = new_length ? write_start : END;
   }
   else {
      node_work1.m_data.m_start = old_start;
   }
   node_work1.m_data.m_length = new_length;
   node_work1.m_data.m_bytes = new_bytes;

   if (!data_update_rec(root.m_data_root, key, &node_work1, &newroot)) {
      return false;
   }

   root.m_data_root = newroot;
   return true;
}

//! @brief Allocate a catalog node and optional payload storage for a new entry.
static bool allocate_storage(NarfSector length, NarfSector *metaref, NarfSector *start) {
   NarfSector freeref;
   NarfSector newroot;
   NarfSector removed_ref;
   FreePayload removed_free;
   NarfSector free_start;
   NarfSector free_length;

   if (length > 0 && free_best_rec(root.m_free_root, length, &freeref, &node_work1)) {
      free_start = node_work1.m_free.m_start;
      free_length = node_work1.m_free.m_length;
      if (!free_delete_rec(root.m_free_root, free_length, free_start, freeref,
                           &newroot, &removed_ref, &removed_free)) return false;
      root.m_free_root = newroot;
      (void) removed_ref;
      if (!alloc_node_sector(metaref)) return false;
      *start = removed_free.m_start;
      if (removed_free.m_length > length) {
         if (!insert_free_extent(removed_free.m_start + length,
                                 removed_free.m_length - length)) return false;
      }
      return true;
   }

   if (root.m_top < root.m_bottom) return false;
   if (length > root.m_top - root.m_bottom) return false;
   if (root.m_top - root.m_bottom - length < 1) return false;
   if (!transaction_may_use_reserve &&
       root.m_top - root.m_bottom - length <= metadata_reserve()) return false;
   if (!alloc_node_sector(metaref)) return false;
   *start = root.m_bottom;
   root.m_bottom += length;
   return true;
}

//! @brief Validate a referenced AVL tree by walking only one 512-byte node at a time.
static bool validate_tree_rec_depth(NarfSector ref, unsigned depth) {
   NarfSector left;
   NarfSector right;

   if (ref_is_null(ref)) return true;
   if (depth > NARF_MAX_AVL_DEPTH) return false;
   if (!read_node(ref, &node_work0)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;

   if (!validate_tree_rec_depth(left, depth + 1)) return false;
   return validate_tree_rec_depth(right, depth + 1);
}

//! @brief Validate a referenced AVL tree from its root.
static bool validate_tree_rec(NarfSector ref) {
   return validate_tree_rec_depth(ref, 0);
}

//! @brief Validate the mounted root and its authoritative metadata trees.
static bool validate_mounted_root(void) {
   if (!verify()) return false;
   if (root.m_total_sectors < NARF_MIN_FS_SECTORS) return false;
   if (root.m_bottom < 2) return false;
   if (root.m_top > root.m_total_sectors) return false;
   if (root.m_bottom > root.m_top) return false;

   if (!validate_tree_rec(root.m_data_root)) return false;
   if (!validate_tree_rec(root.m_free_root)) return false;

   return true;
}

//! @brief Try to mount one root copy and reject roots pointing at torn authoritative nodes.
static bool mount_root_copy(NarfSector start, int which) {
   if (!read_root_copy(start, which, &root_tmp)) return false;
   root_from_disk(&root_tmp);

   if (!validate_mounted_root()) return false;

   root_copy = which;
   lfsr_state = root.m_lfsr_seed;
   if (lfsr_state == 0) {
      lfsr_state = 0x1f2e3d4c;
   }

   return true;
}

//! @brief Recycle metadata-node trash after a mutation commits successfully.
static void move_trash_to_spare_after_commit(void) {
   RootSnapshot committed;
   unsigned count = trash_node_count;

   if (count == 0) {
      trash_node_overflow = false;
      return;
   }

   committed.m_root = root;
   committed.m_lfsr_state = lfsr_state;

   for (unsigned i = 0; i < count; i++) {
      if (!push_spare(trash_nodes[i])) {
         root = committed.m_root;
         lfsr_state = committed.m_lfsr_state;
         trash_node_count = 0;
         trash_node_overflow = false;
         return;
      }
   }

   if (!commit_root()) {
      root = committed.m_root;
      lfsr_state = committed.m_lfsr_state;
   }

   trash_node_count = 0;
   trash_node_overflow = false;
}

//! @brief Commit a public mutation.
static bool commit_user_transaction(void) {
   if (!commit_root()) return false;
   move_trash_to_spare_after_commit();
   return true;
}

#ifdef NARF_MBR_UTILS
// this comes from bootloader.bin, and includes
// everything up to the string to print.
// we didn't include tools to autogenerate it
// because it's not really core NARF stuff, and
// not everyone has "nasm" installed.
// refer to the Makefile for building it.
static const uint8_t boot_code_stub[] = {
   0xeb, 0x00, 0xb8, 0xc0, 0x07, 0x8e, 0xd8, 0x8e,
   0xc0, 0xbe, 0x21, 0x7c, 0xe8, 0x02, 0x00, 0xeb,
   0xfe, 0xac, 0x08, 0xc0, 0x74, 0x05, 0xe8, 0x03,
   0x00, 0xeb, 0xf6, 0xc3, 0xb4, 0x0e, 0xcd, 0x10,
   0xc3 };
static const char boot_code_msg[] = "NARF! not bootable.\r\n";

//! @brief Write a basic MBR sector containing NARF partition support.
bool narf_mbr(const char *message) {
   MBR *mbr = (MBR *) buffer;
   size_t len;
   size_t max_msg;

   if (!narf_io_open()) return false;
   if (message == NULL) message = boot_code_msg;
   max_msg = sizeof(mbr->boot_code) - sizeof(boot_code_stub);
   len = strlen(message);
   if (len + 1 > max_msg) return false;

   memset(buffer, 0, sizeof(buffer));
   memcpy(mbr->boot_code, boot_code_stub, sizeof(boot_code_stub));
   memcpy(mbr->boot_code + sizeof(boot_code_stub), message, len + 1);
   mbr->signature = MBR_SIGNATURE;
   return narf_io_write(0, buffer);
}

//! @brief Create or replace a NARF MBR partition entry.
bool narf_partition(int partition) {
   int i;
   NarfSector start;
   NarfSector end;
   NarfSector sectors;
   MBR *mbr;

   if (!narf_io_open()) return false;
   if (partition < 1 || partition > 4) return false;
   sectors = narf_io_sectors();
   start = 2048;
   end = sectors;
   if (sectors < start + NARF_MIN_FS_SECTORS) return false;

   mbr = (MBR *) buffer;
   if (!narf_io_read(0, buffer)) return false;
   --partition;

   for (i = 0; i < 4; i++) {
      if (i < partition && mbr->partitions[i].partition_type) {
         start = mbr->partitions[i].start_lba + mbr->partitions[i].partition_size;
      }
      else if (i > partition && mbr->partitions[i].partition_type) {
         end = mbr->partitions[i].start_lba;
         break;
      }
   }

   if (end <= start || end - start < NARF_MIN_FS_SECTORS) return false;
   mbr->partitions[partition].partition_type = NARF_PART_TYPE;
   mbr->partitions[partition].start_lba = start;
   mbr->partitions[partition].partition_size = end - start;
   return narf_io_write(0, buffer);
}

//! @brief Format an existing NARF MBR partition.
bool narf_format(int partition) {
   MBR *mbr;
   if (!narf_io_open()) return false;
   if (partition < 1 || partition > 4) return false;
   mbr = (MBR *) buffer;
   if (!narf_io_read(0, buffer)) return false;
   --partition;
   if (mbr->partitions[partition].partition_type != NARF_PART_TYPE) return false;
   return narf_mkfs(mbr->partitions[partition].start_lba,
                    mbr->partitions[partition].partition_size);
}

//! @brief Find the first NARF MBR partition entry.
int narf_findpart(void) {
   int i;
   MBR *mbr;
   if (!narf_io_open()) return -1;
   mbr = (MBR *) buffer;
   if (!narf_io_read(0, buffer)) return -1;
   for (i = 0; i < 4; i++) {
      if (mbr->partitions[i].partition_type == NARF_PART_TYPE) return i + 1;
   }
   return -1;
}

//! @brief Mount a NARF MBR partition by number.
bool narf_mount(int partition) {
   MBR *mbr;
   if (!narf_io_open()) return false;
   if (partition < 1 || partition > 4) return false;
   mbr = (MBR *) buffer;
   if (!narf_io_read(0, buffer)) return false;
   --partition;
   if (mbr->partitions[partition].partition_type != NARF_PART_TYPE) return false;
   return narf_init(mbr->partitions[partition].start_lba);
}
#endif

//! @brief Format a NARF filesystem at a sector origin.
bool narf_mkfs(NarfSector start, NarfSector size) {
   if (!narf_io_open()) return false;
   if (size < NARF_MIN_FS_SECTORS) return false;
   if (start > narf_io_sectors()) return false;
   if (size > narf_io_sectors() - start) return false;
   return init_root(start, size);
}

//! @brief Mount a NARF filesystem at a sector origin.
bool narf_init(NarfSector start) {
   bool loval;
   bool hival;
   uint32_t loversion = 0;
   uint32_t hiversion = 0;
   int chosen;
   int fallback;

   if (!narf_io_open()) return false;
   loval = read_root_copy_version(start, 0, &loversion);
   hival = read_root_copy_version(start, 1, &hiversion);

   if (loval && hival) {
      chosen = version_after(hiversion, loversion) ? 1 : 0;
      fallback = 1 - chosen;
      if (mount_root_copy(start, chosen)) return true;
      return mount_root_copy(start, fallback);
   }

   if (loval) return mount_root_copy(start, 0);
   if (hival) return mount_root_copy(start, 1);

   return false;
}

//! @brief Return basic filesystem capacity and key-count statistics.
bool narf_stat(NarfStat *stats) {
   NarfSector free_sectors;

   if (stats == NULL) return false;
   if (!verify()) return false;
   if (root.m_bottom > root.m_top) return false;

   memset(stats, 0, sizeof(*stats));

   free_sectors = root.m_top - root.m_bottom;
   if (!free_sector_count_rec(root.m_free_root, &free_sectors)) return false;
   if (free_sectors > root.m_total_sectors) return false;

   stats->total_sectors = root.m_total_sectors;
   stats->free_sectors = free_sectors;
   stats->used_sectors = root.m_total_sectors - free_sectors;
   stats->file_count = root.m_count;
   stats->max_key_bytes = KEYSIZE - 1;

   return true;
}


typedef struct {
   NarfFsckReport m_report;
   char m_prev_key[KEYSIZE];
   bool m_have_prev_key;
   FreePayload m_prev_free;
   NarfSector m_prev_free_sector;
   bool m_have_prev_free;
} FsckContext;

static FsckContext fsck_ctx;
static bool fsck_deep_checks = false;

//! @brief Record one fsck error without aborting the whole scan.
static void fsck_error(void) {
   if (fsck_ctx.m_report.errors != (NarfSector) -1) {
      fsck_ctx.m_report.errors++;
   }
}

//! @brief Return true when two non-empty extents overlap.
static bool extents_overlap(NarfSector a_start, NarfSector a_len,
                            NarfSector b_start, NarfSector b_len) {
   NarfSector a_end;
   NarfSector b_end;

   if (a_len == 0 || b_len == 0) return false;
   if (a_start == END || b_start == END) return false;
   if (a_len > ((NarfSector) -1) - a_start) return true;
   if (b_len > ((NarfSector) -1) - b_start) return true;

   a_end = a_start + a_len;
   b_end = b_start + b_len;
   return a_start < b_end && b_start < a_end;
}

//! @brief Validate AVL shape, stored heights, and balance factors.
static int fsck_tree_shape_rec(NarfSector ref, bool free_tree, unsigned depth) {
   NarfSector left;
   NarfSector right;
   int lh;
   int rh;
   int expected;
   uint8_t stored_height;

   if (ref_is_null(ref)) return 0;

   if (depth > NARF_MAX_AVL_DEPTH) {
      fsck_error();
      return 0;
   }

   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return 0;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;
   stored_height = node_work0.m_height;

   if (free_tree) {
      fsck_ctx.m_report.free_nodes++;
   }
   else {
      fsck_ctx.m_report.data_nodes++;
   }

   lh = fsck_tree_shape_rec(left, free_tree, depth + 1);
   rh = fsck_tree_shape_rec(right, free_tree, depth + 1);
   expected = (lh > rh ? lh : rh) + 1;

   if (stored_height != (uint8_t) expected) {
      fsck_error();
   }

   if (lh - rh > 1 || rh - lh > 1) {
      fsck_error();
   }

   return expected;
}

//! @brief Validate data-tree lexical order by in-order traversal.
static void fsck_data_order_rec(NarfSector ref) {
   NarfSector left;
   NarfSector right;

   if (ref_is_null(ref)) return;
   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;

   fsck_data_order_rec(left);

   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   if (fsck_ctx.m_have_prev_key && strcmp(fsck_ctx.m_prev_key, node_work0.m_key) >= 0) {
      fsck_error();
   }

   strncpy(fsck_ctx.m_prev_key, node_work0.m_key, sizeof(fsck_ctx.m_prev_key));
   fsck_ctx.m_prev_key[sizeof(fsck_ctx.m_prev_key) - 1] = 0;
   fsck_ctx.m_have_prev_key = true;

   fsck_data_order_rec(right);
}

//! @brief Validate free-tree ordering by in-order traversal.
static void fsck_free_order_rec(NarfSector ref) {
   NarfSector left;
   NarfSector right;
   FreePayload cur;

   if (ref_is_null(ref)) return;
   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;

   fsck_free_order_rec(left);

   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   cur = node_work0.m_free;

   if (cur.m_length == 0 || cur.m_start == END) {
      fsck_error();
   }
   else {
      fsck_ctx.m_report.free_extents++;
      if (fsck_ctx.m_report.free_sectors <= ((NarfSector) -1) - cur.m_length) {
         fsck_ctx.m_report.free_sectors += cur.m_length;
      }
      else {
         fsck_error();
      }
   }

   if (fsck_ctx.m_have_prev_free) {
      int cmp = free_cmp_values(fsck_ctx.m_prev_free.m_length,
                                fsck_ctx.m_prev_free.m_start,
                                fsck_ctx.m_prev_free_sector,
                                &node_work0, ref);
      if (cmp >= 0) {
         fsck_error();
      }
   }

   fsck_ctx.m_prev_free = cur;
   fsck_ctx.m_prev_free_sector = ref;
   fsck_ctx.m_have_prev_free = true;

   fsck_free_order_rec(right);
}

//! @brief Count matching metadata node sectors in a tree.
static NarfSector fsck_count_node_sector_rec(NarfSector ref, NarfSector sector) {
   NarfSector left;
   NarfSector right;
   NarfSector count = 0;

   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return 0;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;

   if (ref == sector) count++;
   count += fsck_count_node_sector_rec(left, sector);
   count += fsck_count_node_sector_rec(right, sector);
   return count;
}

//! @brief Check whether one free extent overlaps a given payload extent.
static void fsck_scan_free_overlap_rec(NarfSector ref, NarfSector start, NarfSector length) {
   NarfSector left;
   NarfSector right;
   FreePayload fp;

   if (ref_is_null(ref)) return;
   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;
   fp = node_work0.m_free;

   if (extents_overlap(start, length, fp.m_start, fp.m_length)) {
      fsck_error();
   }

   fsck_scan_free_overlap_rec(left, start, length);
   fsck_scan_free_overlap_rec(right, start, length);
}

//! @brief Check whether one data extent overlaps another data extent.
static void fsck_scan_data_overlap_rec(NarfSector ref, NarfSector self,
                                       NarfSector start, NarfSector length) {
   NarfSector left;
   NarfSector right;
   DataPayload dp;

   if (ref_is_null(ref)) return;
   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;
   dp = node_work0.m_data;

   if (ref != self &&
       extents_overlap(start, length, dp.m_start, dp.m_length)) {
      fsck_error();
   }

   fsck_scan_data_overlap_rec(left, self, start, length);
   fsck_scan_data_overlap_rec(right, self, start, length);
}

//! @brief Check whether one free extent overlaps another free extent.
static void fsck_scan_free_free_overlap_rec(NarfSector ref, NarfSector self,
                                            NarfSector start, NarfSector length) {
   NarfSector left;
   NarfSector right;
   FreePayload fp;

   if (ref_is_null(ref)) return;
   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;
   fp = node_work0.m_free;

   if (ref != self &&
       extents_overlap(start, length, fp.m_start, fp.m_length)) {
      fsck_error();
   }

   fsck_scan_free_free_overlap_rec(left, self, start, length);
   fsck_scan_free_free_overlap_rec(right, self, start, length);
}

//! @brief Validate data payload ranges and cross-tree extent overlaps.
static void fsck_data_extents_rec(NarfSector ref) {
   NarfSector left;
   NarfSector right;
   DataPayload dp;
   NarfSector needed;

   if (ref_is_null(ref)) return;
   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;
   dp = node_work0.m_data;

   if (dp.m_length == 0) {
      if (dp.m_start != END || dp.m_bytes != 0) {
         fsck_error();
      }
   }
   else {
      needed = BYTES2SECTORS(dp.m_bytes);
      if (dp.m_start == END || dp.m_start < 2 || dp.m_start >= root.m_bottom ||
          dp.m_length > root.m_bottom - dp.m_start || needed != dp.m_length) {
         fsck_error();
      }
      else {
         if (fsck_ctx.m_report.payload_sectors <= ((NarfSector) -1) - dp.m_length) {
            fsck_ctx.m_report.payload_sectors += dp.m_length;
         }
         else {
            fsck_error();
         }
         if (fsck_deep_checks) {
            fsck_scan_free_overlap_rec(root.m_free_root, dp.m_start, dp.m_length);
            fsck_scan_data_overlap_rec(root.m_data_root, ref, dp.m_start, dp.m_length);
         }
      }
   }

   fsck_data_extents_rec(left);
   fsck_data_extents_rec(right);
}

//! @brief Validate free extent ranges and free-free overlaps.
static void fsck_free_extents_rec(NarfSector ref) {
   NarfSector left;
   NarfSector right;
   FreePayload fp;

   if (ref_is_null(ref)) return;
   if (!read_node(ref, &node_work0)) {
      fsck_error();
      return;
   }

   left = node_work0.m_left;
   right = node_work0.m_right;
   fp = node_work0.m_free;

   if (fp.m_length == 0 || fp.m_start == END || fp.m_start < 2 ||
       fp.m_start >= root.m_bottom || fp.m_length > root.m_bottom - fp.m_start) {
      fsck_error();
   }
   else {
      if (fsck_deep_checks) {
         fsck_scan_free_free_overlap_rec(root.m_free_root, ref, fp.m_start, fp.m_length);
      }
   }

   fsck_free_extents_rec(left);
   fsck_free_extents_rec(right);
}

//! @brief Validate root-resident metadata-free stack entries.
static void fsck_spare_stack(void) {
   if (root.m_spare_count > SPARE_MAX) {
      fsck_error();
      return;
   }

   for (NarfSector i = 0; i < root.m_spare_count; i++) {
      NarfSector sector = root.m_spare_inline[i];

      fsck_ctx.m_report.spare_nodes++;

      if (!valid_node_sector(sector)) {
         fsck_error();
         continue;
      }

      if (sector < root.m_top || sector >= root.m_total_sectors) {
         fsck_error();
      }

      for (NarfSector j = i + 1; j < root.m_spare_count; j++) {
         if (root.m_spare_inline[j] == sector) {
            fsck_error();
         }
      }

      if (fsck_count_node_sector_rec(root.m_data_root, sector) != 0 ||
          fsck_count_node_sector_rec(root.m_free_root, sector) != 0) {
         fsck_error();
      }
   }
}

//! @brief Validate the mounted filesystem and return a small consistency report.
static bool narf_fsck_impl(NarfFsckReport *report, bool deep_checks) {
   memset(&fsck_ctx, 0, sizeof(fsck_ctx));
   fsck_deep_checks = deep_checks;

   if (!verify()) {
      fsck_error();
   }
   else {
      if (root.m_total_sectors < NARF_MIN_FS_SECTORS) fsck_error();
      if (root.m_bottom < 2) fsck_error();
      if (root.m_top > root.m_total_sectors) fsck_error();
      if (root.m_bottom > root.m_top) fsck_error();
      if (root.m_bottom <= root.m_top) {
         fsck_ctx.m_report.free_sectors = root.m_top - root.m_bottom;
      }

      NarfSector shape_errors = fsck_ctx.m_report.errors;

      (void) fsck_tree_shape_rec(root.m_data_root, false, 0);
      (void) fsck_tree_shape_rec(root.m_free_root, true, 0);

      if (fsck_ctx.m_report.errors == shape_errors) {
         fsck_ctx.m_have_prev_key = false;
         fsck_data_order_rec(root.m_data_root);

         fsck_ctx.m_have_prev_free = false;
         fsck_free_order_rec(root.m_free_root);

         fsck_data_extents_rec(root.m_data_root);
         fsck_free_extents_rec(root.m_free_root);
         fsck_spare_stack();
      }

      fsck_ctx.m_report.file_count = root.m_count;
      if (root.m_count != fsck_ctx.m_report.data_nodes) {
         fsck_error();
      }
   }

   if (report != NULL) {
      *report = fsck_ctx.m_report;
   }

   fsck_deep_checks = false;
   return fsck_ctx.m_report.errors == 0;
}

//! @brief Validate the mounted filesystem without quadratic overlap scans.
bool narf_fsck(NarfFsckReport *report) {
   return narf_fsck_impl(report, false);
}

//! @brief Validate the mounted filesystem with full overlap scans.
bool narf_fsck_deep(NarfFsckReport *report) {
   return narf_fsck_impl(report, true);
}

//! @brief Return whether a key exists in the data tree.
bool narf_find(const char *key) {
   return valid_key(key) && verify() && data_find_ref_rec(root.m_data_root, key, NULL, NULL);
}

//! @brief Return the directory prefix after an optional leading separator.
static const char *dir_prefix(const char *dirname, const char *sep) {
   size_t sep_len = strlen(sep);

   if (sep_len != 0 && !strncmp(dirname, sep, sep_len)) {
      return dirname + sep_len;
   }

   return dirname;
}

//! @brief Return whether a key is an immediate entry of a directory.
static bool dir_match(const char *key, const char *dirname, const char *sep) {
   const char *prefix = dir_prefix(dirname, sep);
   size_t prefix_len = strlen(prefix);
   size_t sep_len = strlen(sep);
   const char *p;

   if (strncmp(prefix, key, prefix_len)) return false;
   p = strstr(key + prefix_len, sep);
   return p == NULL || p[sep_len] == 0;
}

//! @brief Build the exclusive lexicographic upper bound for a prefix.
static bool prefix_upper_bound(const char *prefix, char *out, size_t out_size) {
   size_t len;

   if (prefix == NULL || out == NULL || out_size == 0) return false;

   len = strlen(prefix);
   if (len == 0 || len >= out_size) return false;

   memcpy(out, prefix, len + 1);

   while (len > 0) {
      unsigned char c = (unsigned char) out[len - 1];

      if (c != 0xff) {
         out[len - 1] = (char)(c + 1);
         out[len] = 0;
         return true;
      }

      len--;
   }

   return false;
}

//! @brief Scan the data tree for the next directory entry after a key.
static bool dir_scan_rec(NarfSector ref, const char *dirname, const char *sep,
                         const char *prefix, const char *prefix_high,
                         const char *after, const char **best) {
   int cmp;

   if (best == NULL) return false;
   if (ref_is_null(ref)) return true;
   if (!read_node(ref, &node_work0)) return false;

   if (after != NULL && strcmp(node_work0.m_key, after) <= 0) {
      return dir_scan_rec(node_work0.m_right, dirname, sep, prefix, prefix_high,
                          after, best);
   }

   cmp = strcmp(node_work0.m_key, prefix);
   if (cmp < 0) {
      return dir_scan_rec(node_work0.m_right, dirname, sep, prefix, prefix_high,
                          after, best);
   }

   if (prefix_high != NULL && strcmp(node_work0.m_key, prefix_high) >= 0) {
      return dir_scan_rec(node_work0.m_left, dirname, sep, prefix, prefix_high,
                          after, best);
   }

   if (!dir_scan_rec(node_work0.m_left, dirname, sep, prefix, prefix_high,
                     after, best)) {
      return false;
   }

   if (!read_node(ref, &node_work0)) return false;

   if ((*best == NULL || strcmp(node_work0.m_key, *best) < 0) &&
       dir_match(node_work0.m_key, dirname, sep)) {
      strncpy(dir_key, node_work0.m_key, sizeof(dir_key));
      dir_key[sizeof(dir_key) - 1] = 0;
      *best = dir_key;
   }

   if (*best != NULL && strcmp(*best, node_work0.m_key) <= 0) {
      return true;
   }

   return dir_scan_rec(node_work0.m_right, dirname, sep, prefix, prefix_high,
                       after, best);
}

//! @brief Return the next directory entry after an optional previous key.
static const char *dir_scan_next(const char *dirname, const char *sep, const char *after) {
   const char *best = NULL;
   const char *prefix = dir_prefix(dirname, sep);
   const char *prefix_high = NULL;

   if (prefix_upper_bound(prefix, key_work, sizeof(key_work))) {
      prefix_high = key_work;
   }

   if (!dir_scan_rec(root.m_data_root, dirname, sep, prefix, prefix_high, after, &best)) {
      return NULL;
   }

   return best;
}

//! @brief Return the first immediate key in a directory.
const char *narf_dirfirst(const char *dirname, const char *sep) {
   if (!verify()) return NULL;
   if (!valid_dir_args(dirname, sep)) return NULL;
   return dir_scan_next(dirname, sep, NULL);
}

//! @brief Return the immediate directory key after a previous key.
const char *narf_dirnext(const char *dirname, const char *sep, const char *previous_key) {
   if (!verify()) return NULL;
   if (!valid_dir_args(dirname, sep)) return NULL;
   if (!valid_key(previous_key)) return NULL;
   return dir_scan_next(dirname, sep, previous_key);
}

//! @brief Create a key with zero-filled payload storage.
bool narf_alloc(const char *key, NarfByteSize bytes) {
   NarfSector length;
   NarfSector start = END;
   NarfSector metaref;
   NarfSector written;
   NarfSector newroot;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (narf_find(key)) return false;

   transaction_begin();
   length = BYTES2SECTORS(bytes);
   if (!allocate_storage(length, &metaref, &start)) {
      transaction_rollback();
      return false;
   }

   if (!zero_extent(start, length)) {
      transaction_rollback();
      return false;
   }

   memset(&node_work1, 0, sizeof(node_work1));
   node_work1.m_left = END;
   node_work1.m_right = END;
   node_work1.m_data.m_start = length ? start : END;
   node_work1.m_data.m_length = length;
   node_work1.m_data.m_bytes = bytes;
   node_work1.m_height = 1;
   strcpy(node_work1.m_key, key);

   if (!write_node(metaref, &node_work1, &written) ||
       !data_insert_rec(root.m_data_root, written, key, &newroot)) {
      transaction_rollback();
      return false;
   }

   root.m_data_root = newroot;
   root.m_count++;
   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }
   return true;
}

//! @brief Resize an existing key.
bool narf_realloc(const char *key, NarfByteSize bytes) {
   NarfSector newroot;
   NarfSector old_start;
   NarfSector old_length;
   NarfByteSize old_bytes;
   NarfSector new_length;
   NarfSector free_start;
   NarfSector free_length;

   if (!verify()) return false;
   if (!valid_key(key)) return false;

   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) {
      return narf_alloc(key, bytes);
   }

   old_bytes = node_work1.m_data.m_bytes;
   if (bytes > old_bytes) {
      return narf_write(key, NULL, bytes - old_bytes, old_bytes);
   }

   old_start = node_work1.m_data.m_start;
   old_length = node_work1.m_data.m_length;
   new_length = BYTES2SECTORS(bytes);

   transaction_begin();

   if (bytes == 0) {
      if (old_length != 0) {
         if (old_start == END) {
            transaction_rollback();
            return false;
         }

         if (!insert_free_extent(old_start, old_length)) {
            transaction_rollback();
            return false;
         }
      }

      if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) {
         transaction_rollback();
         return false;
      }
      node_work1.m_data.m_start = END;
      node_work1.m_data.m_length = 0;
      node_work1.m_data.m_bytes = 0;
   }
   else if (new_length < old_length) {
      if (old_start == END) {
         transaction_rollback();
         return false;
      }

      free_start = old_start + new_length;
      free_length = old_length - new_length;

      if (free_start < old_start) {
         transaction_rollback();
         return false;
      }

      if (!insert_free_extent(free_start, free_length)) {
         transaction_rollback();
         return false;
      }

      if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) {
         transaction_rollback();
         return false;
      }
      node_work1.m_data.m_length = new_length;
      node_work1.m_data.m_bytes = bytes;
   }
   else {
      if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) {
         transaction_rollback();
         return false;
      }
      node_work1.m_data.m_bytes = bytes;
   }

   if (!data_update_rec(root.m_data_root, key, &node_work1, &newroot)) {
      transaction_rollback();
      return false;
   }

   root.m_data_root = newroot;


   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Compatibility wrapper around narf_realloc().
bool narf_realloc_key(const char *key, NarfByteSize bytes) {
   return narf_realloc(key, bytes);
}

//! @brief Delete a key and return its payload extent to free storage.
bool narf_free(const char *key) {
   NarfSector removed_ref;
   NarfSector newroot;
   DataPayload removed_data;
   NarfSector removed_start;
   NarfSector removed_length;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   transaction_begin();
   transaction_may_use_reserve = true;
   if (!data_delete_rec(root.m_data_root, key, &newroot, &removed_ref, &removed_data)) {
      transaction_rollback();
      return false;
   }
   root.m_data_root = newroot;
   removed_start = removed_data.m_start;
   removed_length = removed_data.m_length;
   if (!insert_free_extent_with_ref(removed_ref, removed_start, removed_length)) {
      transaction_rollback();
      return false;
   }
   if (root.m_count) root.m_count--;
   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }
   return true;
}

//! @brief Compatibility wrapper around narf_free().
bool narf_free_key(const char *key) {
   return narf_free(key);
}

//! @brief Rename one key without moving its payload extent.
bool narf_rename_key(const char *key, const char *newkey) {
   NarfSector removed_ref;
   NarfSector newroot;
   NarfSector written;
   DataPayload renamed_data;

   if (!verify()) return false;
   if (!valid_key(key) || !valid_key(newkey)) return false;
   if (narf_find(newkey)) return false;
   if (strcmp(key, newkey) == 0) return true;
   transaction_begin();
   if (!data_delete_rec(root.m_data_root, key, &newroot, &removed_ref, &renamed_data)) {
      transaction_rollback();
      return false;
   }
   root.m_data_root = newroot;
   memset(&node_work1, 0, sizeof(node_work1));
   node_work1.m_data = renamed_data;
   node_work1.m_left = END;
   node_work1.m_right = END;
   node_work1.m_height = 1;
   strncpy(node_work1.m_key, newkey, sizeof(node_work1.m_key));
   node_work1.m_key[sizeof(node_work1.m_key) - 1] = 0;
   if (!write_node(removed_ref, &node_work1, &written) ||
       !data_insert_rec(root.m_data_root, written, newkey, &newroot)) {
      transaction_rollback();
      return false;
   }
   root.m_data_root = newroot;
   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }
   return true;
}

//! @brief Rename one directory prefix and all keys below it in one transaction.
bool narf_rename_dir(const char *oldkey, const char *newkey, const char *sep) {
   NarfSector removed_ref;
   NarfSector newroot;
   NarfSector written;
   NarfSector ref;
   NarfSector best;
   DataPayload renamed_data;
   size_t oldlen;
   size_t newlen;
   size_t suffixlen;
   int cmp;
   bool renamed_any = false;

   if (!verify()) return false;
   if (!valid_key(oldkey) || !valid_key(newkey)) return false;
   if (!valid_dir_key(oldkey, sep) || !valid_dir_key(newkey, sep)) return false;

   oldlen = strlen(oldkey);
   newlen = strlen(newkey);

   if (strcmp(oldkey, newkey) == 0) {
      return narf_find(oldkey);
   }

   if (newlen > oldlen && strncmp(newkey, oldkey, oldlen) == 0) {
      return false;
   }

   /* Reject an existing destination subtree.  Lower-bound search for newkey. */
   ref = root.m_data_root;
   best = END;
   while (!ref_is_null(ref)) {
      if (!read_node(ref, &node_work0)) return false;
      cmp = strcmp(node_work0.m_key, newkey);
      if (cmp < 0) {
         ref = node_work0.m_right;
      }
      else {
         best = ref;
         ref = node_work0.m_left;
      }
   }
   if (!ref_is_null(best)) {
      if (!read_node(best, &node_work0)) return false;
      if (strncmp(node_work0.m_key, newkey, newlen) == 0) return false;
   }

   transaction_begin();

   for (;;) {
      /* Find the lexicographically first key at or below oldkey. */
      ref = root.m_data_root;
      best = END;
      while (!ref_is_null(ref)) {
         if (!read_node(ref, &node_work0)) {
            transaction_rollback();
            return false;
         }
         cmp = strcmp(node_work0.m_key, oldkey);
         if (cmp < 0) {
            ref = node_work0.m_right;
         }
         else {
            best = ref;
            ref = node_work0.m_left;
         }
      }

      if (ref_is_null(best)) break;
      if (!read_node(best, &node_work0)) {
         transaction_rollback();
         return false;
      }
      if (strncmp(node_work0.m_key, oldkey, oldlen) != 0) break;

      strncpy(dir_key, node_work0.m_key, sizeof(dir_key));
      dir_key[sizeof(dir_key) - 1] = 0;

      suffixlen = strlen(dir_key + oldlen);
      if (newlen + suffixlen >= sizeof(key_work)) {
         transaction_rollback();
         return false;
      }
      memcpy(key_work, newkey, newlen);
      memcpy(key_work + newlen, dir_key + oldlen, suffixlen + 1);

      if (!data_delete_rec(root.m_data_root, dir_key, &newroot,
                           &removed_ref, &renamed_data)) {
         transaction_rollback();
         return false;
      }
      root.m_data_root = newroot;

      memset(&node_work1, 0, sizeof(node_work1));
      node_work1.m_data = renamed_data;
      node_work1.m_left = END;
      node_work1.m_right = END;
      node_work1.m_height = 1;
      strncpy(node_work1.m_key, key_work, sizeof(node_work1.m_key));
      node_work1.m_key[sizeof(node_work1.m_key) - 1] = 0;

      if (!write_node(END, &node_work1, &written) ||
          !data_insert_rec(root.m_data_root, written, key_work, &newroot)) {
         transaction_rollback();
         return false;
      }
      root.m_data_root = newroot;
      renamed_any = true;
   }

   if (!renamed_any) {
      transaction_rollback();
      return false;
   }

   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }
   return true;
}

//! @brief Return the physical sector of a key payload.
NarfSector narf_sector(const char *key) {
   if (!verify()) return END;
   if (!valid_key(key)) return END;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) return END;
   if (node_work1.m_data.m_start == END || node_work1.m_data.m_length == 0) return END;
   if (node_work1.m_data.m_start >= root.m_total_sectors) return END;
   if (node_work1.m_data.m_length > root.m_total_sectors - node_work1.m_data.m_start) return END;
   return root.m_origin + node_work1.m_data.m_start;
}

//! @brief Return the byte size of a key payload.
NarfByteSize narf_size(const char *key) {
   if (!verify()) return 0;
   if (!valid_key(key)) return 0;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) return 0;
   return node_work1.m_data.m_bytes;
}

//! @brief Return a copy of a key metadata area.
void *narf_metadata(const char *key) {
   static uint8_t metadata[NARF_METADATA_SIZE];
   if (!verify()) return NULL;
   if (!valid_key(key)) return NULL;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) return NULL;
   memcpy(metadata, node_work1.m_data.m_metadata, sizeof(metadata));
   return metadata;
}

//! @brief Replace a key metadata area.
bool narf_set_metadata(const char *key, void *data) {
   NarfSector newroot;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (data == NULL) return false;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) return false;
   transaction_begin();
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) {
      transaction_rollback();
      return false;
   }
   memcpy(node_work1.m_data.m_metadata, data, sizeof(node_work1.m_data.m_metadata));
   if (!data_update_rec(root.m_data_root, key, &node_work1, &newroot)) {
      transaction_rollback();
      return false;
   }
   root.m_data_root = newroot;
   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }
   return true;
}

//! @brief Atomically write bytes at an offset in a key payload.
bool narf_write(const char *key, const void *data, NarfByteSize size, NarfByteSize offset) {
   NarfSector newroot;
   NarfByteSize write_end;
   NarfByteSize new_bytes;
   NarfByteSize old_bytes;
   NarfSector old_start;
   NarfSector old_length;
   NarfSector new_length;
   NarfSector new_start;
   NarfSector i;
   const uint8_t *src = (const uint8_t *) data;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (size > ((NarfByteSize) -1) - offset) return false;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) return false;

   old_bytes = node_work1.m_data.m_bytes;
   old_start = node_work1.m_data.m_start;
   old_length = node_work1.m_data.m_length;
   write_end = offset + size;
   new_bytes = old_bytes;
   if (write_end > new_bytes) new_bytes = write_end;

   if (size == 0 && new_bytes == old_bytes) {
      return true;
   }

   transaction_begin();

   if (offset == old_bytes && new_bytes > old_bytes) {
      if (write_append_fast(key, src, size, old_bytes, old_start, old_length, new_bytes)) {
         if (!commit_user_transaction()) {
            transaction_rollback();
            return false;
         }
         return true;
      }

      transaction_rollback();
      transaction_begin();
   }

   new_length = BYTES2SECTORS(new_bytes);

   if (!allocate_data_extent(new_length, &new_start)) {
      transaction_rollback();
      return false;
   }

   for (i = 0; i < new_length; i++) {
      NarfByteSize base;
      NarfByteSize sector_bytes = NARF_SECTOR_SIZE;
      NarfByteSize sector_end;

      if (i > ((NarfByteSize) -1) / NARF_SECTOR_SIZE) {
         transaction_rollback();
         return false;
      }

      base = (NarfByteSize) i * NARF_SECTOR_SIZE;

      if (((NarfByteSize) -1) - base < sector_bytes) {
         sector_bytes = ((NarfByteSize) -1) - base;
      }

      sector_end = base + sector_bytes;

      memset(buffer, 0, sizeof(buffer));

      if (base < old_bytes && old_start != END && i < old_length) {
         NarfByteSize old_n = old_bytes - base;

         if (old_n > sector_bytes) {
            old_n = sector_bytes;
         }

         if (!narf_io_read(root.m_origin + old_start + i, buffer)) {
            transaction_rollback();
            return false;
         }

         if (old_n < sizeof(buffer)) {
            memset(buffer + old_n, 0, sizeof(buffer) - old_n);
         }
      }

      if (base < write_end && offset < sector_end) {
         NarfByteSize begin = offset > base ? offset : base;
         NarfByteSize end = write_end < sector_end ? write_end : sector_end;
         NarfByteSize nbytes = end - begin;
         NarfByteSize dest = begin - base;

         if (src != NULL) {
            memcpy(buffer + dest, src + (begin - offset), nbytes);
         }
         else {
            memset(buffer + dest, 0, nbytes);
         }
      }

      if (!narf_io_write(root.m_origin + new_start + i, buffer)) {
         transaction_rollback();
         return false;
      }
   }

   if (old_length > 0) {
      if (!insert_free_extent(old_start, old_length)) {
         transaction_rollback();
         return false;
      }
   }

   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) {
      transaction_rollback();
      return false;
   }

   node_work1.m_data.m_start = new_length ? new_start : END;
   node_work1.m_data.m_length = new_length;
   node_work1.m_data.m_bytes = new_bytes;

   if (!data_update_rec(root.m_data_root, key, &node_work1, &newroot)) {
      transaction_rollback();
      return false;
   }

   root.m_data_root = newroot;


   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Append bytes to a key payload.
bool narf_append(const char *key, const void *data, NarfByteSize size) {
   NarfByteSize old_size;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (data == NULL && size != 0) return false;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, NULL)) return false;

   old_size = narf_size(key);
   if (size > ((NarfByteSize) -1) - old_size) return false;

   return narf_write(key, data, size, old_size);
}

//! @brief Compatibility wrapper around narf_append().
bool narf_append_key(const char *key, const void *data, NarfByteSize size) {
   return narf_append(key, data, size);
}

#ifdef NARF_USE_DEFRAG
//! @brief Find the lowest-addressed real free payload extent.
static bool defrag_lowest_free_rec(NarfSector ref, NarfSector *bestref, FreePayload *bestfree) {
   NarfSector left;
   NarfSector right;

   if (bestref == NULL || bestfree == NULL) return false;

   if (ref_is_null(ref)) {
      return true;
   }

   if (!read_node(ref, &node_work0)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;

   if (node_work0.m_free.m_start < bestfree->m_start) {
      *bestref = ref;
      *bestfree = node_work0.m_free;
   }

   if (!defrag_lowest_free_rec(left, bestref, bestfree)) {
      return false;
   }

   if (!defrag_lowest_free_rec(right, bestref, bestfree)) {
      return false;
   }

   return true;
}

//! @brief Find a real free payload extent by starting sector.
static bool defrag_find_free_start_rec(NarfSector ref, NarfSector start, NarfSector *found, FreePayload *outfree) {
   NarfSector left;
   NarfSector right;
   NarfSector free_start;
   NarfSector free_length;

   if (ref_is_null(ref)) return false;
   if (!read_node(ref, &node_work0)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;
   free_start = node_work0.m_free.m_start;
   free_length = node_work0.m_free.m_length;

   if (free_start == start && free_length != 0) {
      if (found) *found = ref;
      if (outfree) {
         outfree->m_start = free_start;
         outfree->m_length = free_length;
      }
      return true;
   }

   if (defrag_find_free_start_rec(left, start, found, outfree)) return true;
   return defrag_find_free_start_rec(right, start, found, outfree);
}

//! @brief Find a data-tree entry by payload starting sector.
static bool defrag_find_data_start_rec(NarfSector ref, NarfSector start, NarfSector gaplength, NarfSector scratchlen, NarfSector *found,
                                       DataPayload *outdata, char *outkey) {
   NarfSector left;
   NarfSector right;
   NarfSector data_start;
   NarfSector data_length;

   if (ref_is_null(ref)) return false;
   if (!read_node(ref, &node_work0)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;
   data_start = node_work0.m_data.m_start;
   data_length = node_work0.m_data.m_length;

#if 0
   if (data_start == start && data_length != 0 && (data_length <= gaplength || data_length <= scratchlen)) {
      if (found) *found = ref;
      if (outdata) *outdata = node_work0.m_data;
      if (outkey) {
         strncpy(outkey, node_work0.m_key, KEYSIZE);
         outkey[KEYSIZE - 1] = 0;
      }
      return true;
   }
#endif
   if (data_start > start && (outdata->m_start == END || data_start > outdata->m_start) && data_length <= gaplength) {
      if (found) *found = ref;
      if (outdata) *outdata = node_work0.m_data;
      if (outkey) {
         strncpy(outkey, node_work0.m_key, KEYSIZE);
         outkey[KEYSIZE - 1] = 0;
      }
   }

   if (defrag_find_data_start_rec(left, start, gaplength, scratchlen, found, outdata, outkey)) return true;
   return defrag_find_data_start_rec(right, start, gaplength, scratchlen, found, outdata, outkey);
}

//! @brief Copy payload sectors between non-overlapping extents chosen by the caller.
static bool defrag_copy_extent(NarfSector src, NarfSector dst, NarfSector length) {
   NarfSector i;

   if (length == 0 || src == dst) return true;
   if (src == END || dst == END) return false;
   if (src > root.m_total_sectors || length > root.m_total_sectors - src) return false;
   if (dst > root.m_total_sectors || length > root.m_total_sectors - dst) return false;

   for (i = 0; i < length; i++) {
      if (!narf_io_read(root.m_origin + src + i, buffer)) return false;
      if (!narf_io_write(root.m_origin + dst + i, buffer)) return false;
   }

   return true;
}

//! @brief Update one data node after its payload extent has moved.
static bool defrag_update_data_start(const char *key, NarfSector start) {
   NarfSector newroot;

   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) return false;
   node_work1.m_data.m_start = start;

   if (!data_update_rec(root.m_data_root, key, &node_work1, &newroot)) return false;
   root.m_data_root = newroot;
   return true;
}

//! @brief Carve unused tail sectors from overlong payload extents.
static bool defrag_carve_rec(NarfSector ref, bool *changed) {
   NarfSector left;
   NarfSector right;
   NarfSector data_start;
   NarfSector data_length;
   NarfByteSize data_bytes;
   NarfSector needed;
   NarfSector free_start;
   NarfSector free_length;
   NarfSector newroot;

   if (ref_is_null(ref)) return true;
   if (changed == NULL) return false;

   if (!read_node(ref, &node_work0)) return false;
   left = node_work0.m_left;

   if (!defrag_carve_rec(left, changed)) return false;
   if (*changed) return true;

   if (!read_node(ref, &node_work0)) return false;
   right = node_work0.m_right;

   data_start = node_work0.m_data.m_start;
   data_length = node_work0.m_data.m_length;
   data_bytes = node_work0.m_data.m_bytes;
   strncpy(key_work, node_work0.m_key, sizeof(key_work));
   key_work[sizeof(key_work) - 1] = 0;

   needed = BYTES2SECTORS(data_bytes);
   if (needed < data_length) {
      transaction_begin();
      transaction_may_use_reserve = true;

      if (!data_find_ref_rec(root.m_data_root, key_work, NULL, &node_work1)) {
         transaction_rollback();
         return false;
      }

      if (needed == 0) {
         free_start = data_start;
         free_length = data_length;
         node_work1.m_data.m_start = END;
         node_work1.m_data.m_length = 0;
      }
      else {
         free_start = data_start + needed;
         free_length = data_length - needed;
         node_work1.m_data.m_length = needed;
      }

      if (free_length != 0 && !insert_free_extent(free_start, free_length)) {
         transaction_rollback();
         return false;
      }

      if (!data_update_rec(root.m_data_root, key_work, &node_work1, &newroot)) {
         transaction_rollback();
         return false;
      }
      root.m_data_root = newroot;


      if (!commit_user_transaction()) {
         transaction_rollback();
         return false;
      }

      *changed = true;
      return true;
   }

   return defrag_carve_rec(right, changed);
}

//! @brief Run one carve pass over the data tree.
static bool defrag_carve_once(bool *changed) {
   if (changed == NULL) return false;
   *changed = false;
   return defrag_carve_rec(root.m_data_root, changed);
}

//! @brief Merge two adjacent free payload extents.
static bool defrag_merge_free(NarfSector leftref, const FreePayload *left, NarfSector rightref, const FreePayload *right) {
   NarfSector newroot;
   NarfSector removed_ref;
   NarfSector right_removed_ref;
   NarfSector left_start;
   NarfSector left_length;
   NarfSector right_start;
   NarfSector right_length;
   NarfSector new_length;

   if (left == NULL || right == NULL) return false;

   left_start = left->m_start;
   left_length = left->m_length;
   right_start = right->m_start;
   right_length = right->m_length;

   if (left_start == END || right_start == END) return false;
   if (left_start + left_length != right_start) return false;
   if (right_length > ((NarfSector) -1) - left_length) return false;

   transaction_begin();
   transaction_may_use_reserve = true;
   new_length = left_length + right_length;

   if (!free_delete_rec(root.m_free_root, left_length, left_start,
                        leftref, &newroot, &removed_ref, NULL)) {
      transaction_rollback();
      return false;
   }
   root.m_free_root = newroot;

   if (!defrag_find_free_start_rec(root.m_free_root, right_start, &rightref, NULL)) {
      transaction_rollback();
      return false;
   }

   if (!free_delete_rec(root.m_free_root, right_length, right_start,
                        rightref, &newroot, &right_removed_ref, NULL)) {
      transaction_rollback();
      return false;
   }
   root.m_free_root = newroot;

   if (!insert_free_extent_with_ref(removed_ref, left_start, new_length)) {
      transaction_rollback();
      return false;
   }

   if (!insert_free_extent_with_ref(right_removed_ref, END, 0)) {
      transaction_rollback();
      return false;
   }

   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Move adjacent data into a large enough hole, or to the payload frontier.
static bool defrag_move_data_after_free(NarfSector freeref, const FreePayload *free_node, const DataPayload *data_node, const char *data_key) {
   NarfSector newroot;
   NarfSector removed_ref;
   NarfSector free_start;
   NarfSector free_length;
   NarfSector old_start;
   NarfSector old_length;
   NarfSector new_start;
   NarfSector new_free_start;
   NarfSector new_free_length;
   bool nonadjacent = false;

   if (free_node == NULL || data_node == NULL || data_key == NULL) return false;

   free_start = free_node->m_start;
   free_length = free_node->m_length;
   old_start = data_node->m_start;
   old_length = data_node->m_length;

   if (free_start == END || old_start == END) return false;
   //if (free_start + free_length != old_start) return false;

   if (free_length >= old_length) {
      // The destination is entirely inside the existing free hole.
      new_start = free_start;
      new_free_start = free_start + old_length;
      if (free_start + free_length == old_start) {
         // adjacent case
         new_free_length = free_length /* + old_length - old_length */;
      }
      else {
         // non adjacent case
         new_free_length = free_length - old_length;
         nonadjacent = true;
      }
   }
   else {
      // Do not slide through an overlapping too-small hole.  Copy to the
      // open frontier so the committed old payload remains intact until commit.
      if (root.m_bottom > root.m_top) return false;
      if (old_length > root.m_top - root.m_bottom) return false;
      new_start = root.m_bottom;
      new_free_start = free_start;
      new_free_length = free_length + old_length;
      if (new_free_length < free_length) return false;
   }

   if (!defrag_copy_extent(old_start, new_start, old_length)) {
      return false;
   }

   transaction_begin();
   transaction_may_use_reserve = true;

   // TODO FIX removed_ref may not be used...
   if (!free_delete_rec(root.m_free_root, free_length, free_start,
                        freeref, &newroot, &removed_ref, NULL)) {
      transaction_rollback();
      return false;
   }
   root.m_free_root = newroot;

   if (free_length < old_length) {
      root.m_bottom += old_length;
   }

   if (!defrag_update_data_start(data_key, new_start)) {
      transaction_rollback();
      return false;
   }

   if (new_free_start + new_free_length == root.m_bottom) {
      root.m_bottom = new_free_start + new_free_length;
   }
   else if (!insert_free_extent_with_ref(removed_ref, new_free_start, new_free_length)) {
      transaction_rollback();
      return false;
   }

   if (nonadjacent) {
      if (old_start + old_length == root.m_bottom) {
         root.m_bottom = old_start;
      }
      else if (!insert_free_extent(old_start, old_length)) {
         transaction_rollback();
         return false;
      }
   }

   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Reclaim a free payload extent that sits at the current data high-water mark.
static bool defrag_lower_bottom(NarfSector freeref, const FreePayload *free_node) {
   NarfSector newroot;
   NarfSector removed_ref;
   NarfSector free_start;
   NarfSector free_length;

   if (free_node == NULL) return false;
   free_start = free_node->m_start;
   free_length = free_node->m_length;

   if (free_start == END) return false;
   if (free_start + free_length != root.m_bottom) return false;

   transaction_begin();
   transaction_may_use_reserve = true;

   if (!free_delete_rec(root.m_free_root, free_length, free_start,
                        freeref, &newroot, &removed_ref, NULL)) {
      transaction_rollback();
      return false;
   }

   root.m_free_root = newroot;
   root.m_bottom = free_start;

   if (!insert_free_extent_with_ref(removed_ref, END, 0)) {
      transaction_rollback();
      return false;
   }

   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Perform one power-loss-safe payload squish step.
static bool defrag_squish_once(bool *changed) {
   NarfSector freeref = END;
   NarfSector adjref;
   FreePayload free_node = INIT_DEFRAG_SEARCH;
   FreePayload adj_free;
   DataPayload adj_data;
   NarfSector successor;
   NarfSector succlength;
   bool ret;

   if (changed == NULL) return false;
   *changed = false;

   if (!defrag_lowest_free_rec(root.m_free_root, &freeref, &free_node)) {
      return false;
   }

   if (ref_is_null(freeref) || free_node.m_start == END || free_node.m_length == 0) {
      return true;
   }

   succlength = free_node.m_length;
   successor = free_node.m_start + free_node.m_length;

   if (defrag_find_free_start_rec(root.m_free_root, successor, &adjref, &adj_free)) {
      if (!defrag_merge_free(freeref, &free_node, adjref, &adj_free)) return false;
      *changed = true;
      return true;
   }

   adjref = END;
   adj_data.m_start = END;
   ret = defrag_find_data_start_rec(root.m_data_root, successor, succlength, root.m_top - root.m_bottom, &adjref, &adj_data, key_work);
   if (ret || !ref_is_null(adjref)) {
      if (!defrag_move_data_after_free(freeref, &free_node, &adj_data, key_work)) return false;
      *changed = true;
      return true;
   }

   if (successor == root.m_bottom) {
      if (!defrag_lower_bottom(freeref, &free_node)) return false;
      *changed = true;
      return true;
   }

   return false;
}

//! @brief Reclaim one parked catalog-node sector when it reaches root.m_top.
static bool defrag_tidy_once(bool *changed) {
   NarfSector newroot;
   NarfSector removed_ref;

   if (changed == NULL) return false;
   *changed = false;

   if (!valid_node_sector(root.m_top)) {
      return true;
   }

   transaction_begin();
   transaction_may_use_reserve = true;

   if (!free_delete_rec(root.m_free_root, 0, END, root.m_top,
                        &newroot, &removed_ref, NULL)) {
      transaction_rollback();
      return true;
   }

   root.m_free_root = newroot;
   root.m_top += 1;

   if (!commit_user_transaction()) {
      transaction_rollback();
      return false;
   }

   *changed = true;
   return true;
}

//! @brief Defragment the filesystem when defrag support is enabled.
bool narf_defrag(void) {
   bool changed;

   if (!verify()) return false;

   do {
      if (!defrag_carve_once(&changed)) return false;
   } while (changed);

   do {
      if (!defrag_squish_once(&changed)) return false;
   } while (changed);

   do {
      if (!defrag_tidy_once(&changed)) return false;
   } while (changed);

   return true;
}
#endif

#ifdef NARF_DEBUG

#ifdef USE_UTF8_LINE_DRAWING
#define TREE_HORIZ "━"
#define TREE_VERT  "┃"
#define TREE_UPPER "┏"
#define TREE_LOWER "┗"
#define TREE_NIL   "✖"
#else
#define TREE_HORIZ "-"
#define TREE_VERT  "|"
#define TREE_UPPER "/"
#define TREE_LOWER "\\"
#define TREE_NIL   "(nil)"
#endif

//! @brief Test whether a tree-print pattern bit is set.
static bool tree_pattern_bit(uint64_t pattern, int bit) {
   if (bit < 0 || bit >= 64) return false;
   return (pattern & (((uint64_t) 1) << bit)) != 0;
}

//! @brief Compute the line pattern for a right-child subtree.
static uint64_t tree_right_pattern(uint64_t pattern, int indent) {
   if (indent < 0 || indent >= 63) return pattern;
   return (pattern ^ (((uint64_t) 3) << indent)) & ~((uint64_t) 1);
}

//! @brief Return the visible length of a fixed-size metadata byte buffer.
static size_t metadata_debug_len(const uint8_t metadata[NARF_METADATA_SIZE]) {
   size_t len;

   for (len = 0; len < NARF_METADATA_SIZE; len++) {
      if (metadata[len] == 0) {
         break;
      }
   }

   return len;
}

//! @brief Print a fixed-size metadata byte buffer as an escaped string.
static void print_debug_metadata(const uint8_t metadata[NARF_METADATA_SIZE]) {
   size_t len = metadata_debug_len(metadata);
   size_t i;

   if (len == 0) {
      return;
   }

   printf(" metadata=\"");
   for (i = 0; i < len; i++) {
      uint8_t c = metadata[i];

      if (c == '\\' || c == '"') {
         printf("\\%c", c);
      }
      else if (c >= 32 && c <= 126) {
         putchar(c);
      }
      else {
         printf("\\x%02x", c);
      }
   }
   printf("\"");
}

//! @brief Print one formatted debug-tree node.
static void print_tree_node(NarfSector ref, const Node *n, const char *label) {
   if (label[0] == 'F') {
      printf("'%s' [%08x] F-> start:len=(%08x:%u) h=%u",
             n->m_key, ref,
             n->m_free.m_start, (unsigned)n->m_free.m_length, n->m_height);
   }
   else {
      printf("'%s' [%08x] %s-> start:len=(%08x:%u) bytes=%u h=%u",
             n->m_key, ref, label,
             n->m_data.m_start, (unsigned)n->m_data.m_length, (unsigned)n->m_data.m_bytes, n->m_height);
      print_debug_metadata(n->m_data.m_metadata);
   }
}

//! @brief Print one debug tree sideways, using line-drawing limbs when enabled.
static void print_tree(NarfSector ref, int indent, uint64_t pattern, const char *label) {
   NarfSector left = END;
   NarfSector right = END;
   int i;
   const char *arm;

   if (!ref_is_null(ref)) {
      if (!read_node(ref, &node_work0)) {
         return;
      }

      left = node_work0.m_left;
      print_tree(left, indent + 1, pattern, label);
   }

   for (i = 0; i < indent; i++) {
      if (tree_pattern_bit(pattern, i)) {
         printf(TREE_VERT "  ");
      }
      else {
         printf("   ");
      }
   }

   if (indent) {
      if (tree_pattern_bit(pattern, indent)) {
         arm = TREE_LOWER TREE_HORIZ;
      }
      else {
         arm = TREE_UPPER TREE_HORIZ;
      }
   }
   else {
      arm = TREE_HORIZ TREE_HORIZ;
   }

   if (ref_is_null(ref)) {
      printf("%s%s\n", arm, TREE_NIL);
      return;
   }

   if (!read_node(ref, &node_work0)) {
      return;
   }
   right = node_work0.m_right;

   printf("%s ", arm);
   print_tree_node(ref, &node_work0, label);
   printf("\n");

   print_tree(right, indent + 1, tree_right_pattern(pattern, indent), label);
}

//! @brief Print internal NARF root and tree state.
void narf_debug(void) {
   printf("root.m_signature     = %08x '%.4s'\n", root.m_signature, root.m_sigbytes);
   printf("root.m_narf_version  = %08x\n", root.m_narf_version);
   printf("root.m_root_version  = %u copy=%d\n", root.m_root_version, root_copy);
   printf("root.m_total_sectors = %08x\n", root.m_total_sectors);
   printf("root.m_data_root     = [%08x]\n", root.m_data_root);
   printf("root.m_free_root     = [%08x]\n", root.m_free_root);
   printf("root.m_spare_count   = %u\n", (unsigned)root.m_spare_count);
   printf("root.m_lfsr_seed     = %08x\n", root.m_lfsr_seed);
   printf("root.m_count         = %08x\n", root.m_count);
   printf("root.m_bottom        = %08x\n", root.m_bottom);
   printf("root.m_top           = %08x\n", root.m_top);
   printf("data tree:\n");
   print_tree(root.m_data_root, 0, 0, "D");
   printf("free tree:\n");
   print_tree(root.m_free_root, 0, 0, "F");
}
#endif

#ifdef NARF_DETAILS
//! @brief Stub I/O open used when building the standalone layout-details tool.
bool narf_io_open(void) {
   return false;
}

//! @brief Stub I/O close used when building the standalone layout-details tool.
bool narf_io_close(void) {
   return false;
}

//! @brief Stub I/O sector count used when building the standalone layout-details tool.
uint32_t narf_io_sectors(void) {
   return 0;
}

//! @brief Stub I/O write used when building the standalone layout-details tool.
bool narf_io_write(uint32_t sector, void *data) {
   return false;
}

//! @brief Stub I/O read used when building the standalone layout-details tool.
bool narf_io_read(uint32_t sector, void *data) {
   return false;
}

//! @brief Print compile-time layout details for the standalone details build.
int main(int argc, char **argv) {
   (void) argc;
   (void) argv;

   printf("key size : %d bytes\n", sizeof(((Node *) NULL)->m_key));
}
#endif
