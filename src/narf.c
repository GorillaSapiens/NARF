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

#define SIGNATURE 0x4652414E
#define VERSION 0x00000004
#define END INVALID_NAF
#define NARF_MIN_FS_SECTORS 4

// Uncomment for unicode line drawing characters in debug functions
#define USE_UTF8_LINE_DRAWING

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
   #define static_assert(x,y) _Static_assert(x,y)
#else
   #define static_assert(x,y)
#endif

///////////////////////////////////////////////////////
//! @brief Classic MBR Partition Entry (16 bytes)
typedef struct PACKED {
   uint8_t  boot_indicator;
   uint8_t  start_head;
   uint8_t  start_sector;
   uint8_t  start_cylinder;
   uint8_t  partition_type;
   uint8_t  end_head;
   uint8_t  end_sector;
   uint8_t  end_cylinder;
   uint32_t start_lba;
   uint32_t partition_size;
} MBRPartitionEntry;
static_assert(sizeof(MBRPartitionEntry) == 16, "MBRPartitionEntry wrong size");

#define NARF_PART_TYPE 0x6E

typedef struct PACKED {
   uint8_t boot_code[446];
   MBRPartitionEntry partitions[4];
   uint16_t signature;
} MBR;
static_assert(sizeof(MBR) == 512, "MBR wrong size");
#define MBR_SIGNATURE 0xAA55

typedef struct PACKED {
   NarfSector m_sector;
   uint32_t   m_version;
} NarfRef;

typedef struct PACKED {
   NarfRef m_parent;
   NarfRef m_previous;
   NarfRef m_next;
} IndexRefs;

#define NULL_REF ((NarfRef){ END, 0 })

#define REF_BYTES (sizeof(NarfRef))
#define ROOT_USED (4 + 4 + sizeof(NarfByteSize) + sizeof(NarfSector) + \
                   3 * sizeof(NarfRef) + 4 * sizeof(NarfSector) + \
                   4 + 4 + 4)

typedef struct PACKED {
   union {
      uint32_t m_signature;
      uint8_t  m_sigbytes[4];
   };
   uint32_t m_version;
   NarfByteSize m_sector_size;
   NarfSector   m_total_sectors;
   NarfRef      m_data_root;
   NarfRef      m_free_root;
   NarfRef      m_index_root;
   NarfSector   m_count;
   NarfSector   m_bottom;
   NarfSector   m_top;
   NarfSector   m_origin;
   uint32_t     m_root_version;
   uint32_t     m_lfsr_seed;
   uint8_t      m_reserved[NARF_SECTOR_SIZE - ROOT_USED];
   uint32_t     m_checksum;
} Root;
static_assert(sizeof(Root) == NARF_SECTOR_SIZE, "Root wrong size");

typedef struct {
   union {
      uint32_t m_signature;
      uint8_t  m_sigbytes[4];
   };
   uint32_t     m_version;
   NarfByteSize m_sector_size;
   NarfSector   m_total_sectors;
   NarfRef      m_data_root;
   NarfRef      m_free_root;
   NarfRef      m_index_root;
   NarfSector   m_count;
   NarfSector   m_bottom;
   NarfSector   m_top;
   NarfSector   m_origin;
   uint32_t     m_root_version;
   uint32_t     m_lfsr_seed;
} RootState;

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

#define NODE_USED (2 * sizeof(NarfRef) + 1 + sizeof(DataPayload) + 4 + 4 + 4)

typedef struct PACKED {
   NarfRef      m_left;
   NarfRef      m_right;
   uint8_t      m_height;
   union {
      DataPayload m_data;
      FreePayload m_free;
      IndexRefs   m_index;
   };
   char         m_key[NARF_SECTOR_SIZE - NODE_USED];
   uint32_t     m_root_version;
   uint32_t     m_node_version;
   uint32_t     m_checksum;
} Node;
static_assert(sizeof(Node) == NARF_SECTOR_SIZE, "Node wrong size");

#define BYTES2SECTORS(x) \
   ((((x) / (NARF_SECTOR_SIZE * 2)) + \
   (((x) % (NARF_SECTOR_SIZE * 2)) != 0)) * 2)
#define KEYSIZE (sizeof(((Node *) 0)->m_key))

typedef union {
   uint8_t bytes[NARF_SECTOR_SIZE];
   MBR     mbr;
   Root    root;
   Node    node;
} SectorScratch;

typedef struct {
   RootState m_root;
   uint32_t  m_lfsr_state;
} RootSnapshot;

static SectorScratch sector_work;
#define buffer   sector_work.bytes
#define root_tmp sector_work.root
#define node_tmp sector_work.node

static RootState root;
static RootSnapshot saved_root;
static Node node_work0;
static Node node_work1;
static int root_copy = 0;
static char dir_key[KEYSIZE];
static char key_work[KEYSIZE];
static uint32_t lfsr_state = 1;

static uint32_t transaction_root_version(void);
static void dirty_clear(void);
static void transaction_begin(void);
static void transaction_rollback(void);

//! @brief Compute a CRC-32, should ve zlib compatible
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

//! @brief Return true when a versioned NARF reference is null.
static bool ref_is_null(NarfRef ref) {
   return ref.m_sector == END || ref.m_version == 0;
}

static bool valid_sector_pair(NarfSector sector);
static bool read_node_copy_any(NarfSector sector, int which, Node *out);

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

//! @brief Choose a new node-copy version that does not collide with visible copies.
static uint32_t new_node_version(uint32_t old, NarfSector sector) {
   uint32_t v;
   uint32_t c0 = 0;
   uint32_t c1 = 0;

   if (valid_sector_pair(sector)) {
      if (read_node_copy_any(sector, 0, &node_tmp)) c0 = node_tmp.m_node_version;
      if (read_node_copy_any(sector, 1, &node_tmp)) c1 = node_tmp.m_node_version;
   }

   do {
      v = lfsr_next();
   } while (v == 0 || v == old || v == c0 || v == c1);

   return v;
}

//! @brief Validate that the mounted root looks like a current NARF root.
static bool verify(void) {
   if (root.m_signature != SIGNATURE) return false;
   if (root.m_version != VERSION) return false;
   if (root.m_sector_size != NARF_SECTOR_SIZE) return false;
   return true;
}

//! @brief Validate a public key string.
static bool valid_key(const char *key) {
   if (key == NULL) return false;
   if (strlen(key) >= KEYSIZE) return false;
   return true;
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

//! @brief Validate a two-sector record node slot.
static bool valid_sector_pair(NarfSector sector) {
   if (!verify()) return false;
   if (sector == END) return false;
   if (root.m_total_sectors < 2) return false;
   if (sector < root.m_top) return false;
   if (sector > root.m_total_sectors - 2) return false;
   if ((sector & 1) != (root.m_total_sectors & 1)) return false;
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
   out->m_version = root.m_version;
   out->m_sector_size = root.m_sector_size;
   out->m_total_sectors = root.m_total_sectors;
   out->m_data_root = root.m_data_root;
   out->m_free_root = root.m_free_root;
   out->m_index_root = root.m_index_root;
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
   root.m_version = in->m_version;
   root.m_sector_size = in->m_sector_size;
   root.m_total_sectors = in->m_total_sectors;
   root.m_data_root = in->m_data_root;
   root.m_free_root = in->m_free_root;
   root.m_index_root = in->m_index_root;
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
   if (out->m_version != VERSION) return false;
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
   dirty_clear();
   return true;
}

//! @brief Initialize an empty root block for a new filesystem.
static bool init_root(NarfSector origin, NarfSector size) {
   memset(&root, 0, sizeof(root));
   root.m_signature = SIGNATURE;
   root.m_version = VERSION;
   root.m_sector_size = NARF_SECTOR_SIZE;
   root.m_total_sectors = size;
   root.m_data_root = NULL_REF;
   root.m_free_root = NULL_REF;
   root.m_index_root = NULL_REF;
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

//! @brief Compute the checksum for a record node.
static uint32_t node_checksum(Node *n) {
   uint32_t old = n->m_checksum;
   uint32_t ck;
   n->m_checksum = 0;
   ck = crc32(0, n, NARF_SECTOR_SIZE - sizeof(uint32_t));
   n->m_checksum = old;
   return ck;
}

//! @brief Read either physical copy of a node without requiring a specific version.
static bool read_node_copy_any(NarfSector sector, int which, Node *out) {
   if (out == NULL) return false;
   if (which < 0 || which > 1) return false;
   if (!valid_sector_pair(sector)) return false;
   if (!narf_io_read(root.m_origin + sector + (NarfSector) which, out)) return false;
   if (out->m_checksum != node_checksum(out)) return false;
   if (out->m_node_version == 0) return false;
   return true;
}

//! @brief Read one physical copy of a node matching an exact reference.
static bool read_node_copy(NarfRef ref, int which, Node *out) {
   if (ref_is_null(ref)) return false;
   if (!valid_sector_pair(ref.m_sector)) return false;
   if (!narf_io_read(root.m_origin + ref.m_sector + (NarfSector) which, out)) return false;
   if (out->m_checksum != node_checksum(out)) return false;
   if (out->m_node_version != ref.m_version) return false;
   return true;
}

//! @brief Read the valid physical copy matching an exact node reference.
static bool read_node(NarfRef ref, Node *out, int *which) {
   if (read_node_copy(ref, 0, out)) {
      if (which) *which = 0;
      return true;
   }
   if (read_node_copy(ref, 1, out)) {
      if (which) *which = 1;
      return true;
   }
   return false;
}

//! @brief Read the transaction-dirty physical copy of a node sector.
static bool read_dirty_node_copy(NarfSector sector, uint32_t txver, Node *out, int *which) {
   int i;

   if (out == NULL) return false;

   for (i = 0; i < 2; i++) {
      if (read_node_copy_any(sector, i, &node_tmp) && node_tmp.m_root_version == txver) {
         *out = node_tmp;
         if (which) *which = i;
         return true;
      }
   }

   return false;
}

//! @brief Start a fresh transaction-local dirty state.
//!
//! Dirty nodes are self-identifying on disk: a node whose m_root_version equals
//! transaction_root_version() is the dirty copy for the open transaction.  No
//! RAM table is needed, but keeping this function preserves the existing
//! transaction-boundary call sites.
static void dirty_clear(void) {
}

//! @brief Save the current mutable root state before starting a public transaction.
static void transaction_begin(void) {
   saved_root.m_root = root;
   saved_root.m_lfsr_state = lfsr_state;
   dirty_clear();
}

//! @brief Restore the root state saved by transaction_begin().
static void transaction_rollback(void) {
   root = saved_root.m_root;
   lfsr_state = saved_root.m_lfsr_state;
   dirty_clear();
}

static bool write_node(NarfRef oldref, Node *n, NarfRef *newref) {
   int oldcopy = 1;
   int dest;
   uint32_t txver;
   uint32_t oldver = oldref.m_version;
   NarfSector sector = oldref.m_sector;

   if (n == NULL || newref == NULL) return false;
   if (sector == END) return false;

   txver = transaction_root_version();

   if (read_dirty_node_copy(sector, txver, &node_tmp, &dest)) {
      n->m_node_version = node_tmp.m_node_version;
   }
   else if (oldref.m_version != 0 && read_node(oldref, &node_tmp, &oldcopy)) {
      dest = 1 - oldcopy;
      n->m_node_version = new_node_version(oldver, sector);
   }
   else {
      dest = 0;
      n->m_node_version = new_node_version(0, sector);
   }

   n->m_root_version = txver;
   n->m_checksum = 0;
   n->m_checksum = crc32(0, n, NARF_SECTOR_SIZE - sizeof(uint32_t));

   if (!narf_io_write(root.m_origin + sector + (NarfSector) dest, n)) {
      return false;
   }

   newref->m_sector = sector;
   newref->m_version = n->m_node_version;
   return true;
}

//! @brief Return the AVL height for a referenced node.
static int height(NarfRef ref) {
   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &node_tmp, NULL)) return 0;
   return node_tmp.m_height;
}

//! @brief Recompute an AVL node height from its children.
static void update_height(Node *n) {
   int lh = height(n->m_left);
   int rh = height(n->m_right);
   n->m_height = (uint8_t)((lh > rh ? lh : rh) + 1);
}

//! @brief Compute the AVL balance factor for a referenced node.
static int balance_factor(NarfRef ref) {
   NarfRef left;
   NarfRef right;

   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &node_tmp, NULL)) return 0;
   left = node_tmp.m_left;
   right = node_tmp.m_right;
   return height(left) - height(right);
}

//! @brief Perform a copy-on-write AVL right rotation.
static bool rotate_right(NarfRef yref, NarfRef *out) {
   NarfRef xref;
   NarfRef y2;
   NarfRef x2;

   if (!read_node(yref, &node_work0, NULL)) return false;
   xref = node_work0.m_left;
   if (!read_node(xref, &node_work1, NULL)) return false;

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
static bool rotate_left(NarfRef xref, NarfRef *out) {
   NarfRef yref;
   NarfRef x2;
   NarfRef y2;

   if (!read_node(xref, &node_work0, NULL)) return false;
   yref = node_work0.m_right;
   if (!read_node(yref, &node_work1, NULL)) return false;

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
static bool rebalance(NarfRef ref, NarfRef *out) {
   NarfRef child;
   NarfRef tmp;
   int bf;

   if (ref_is_null(ref)) {
      *out = ref;
      return true;
   }

   bf = balance_factor(ref);

   if (bf > 1) {
      if (!read_node(ref, &node_work0, NULL)) return false;
      child = node_work0.m_left;
      if (balance_factor(child) < 0) {
         if (!rotate_left(child, &tmp)) return false;
         if (!read_node(ref, &node_work0, NULL)) return false;
         node_work0.m_left = tmp;
         update_height(&node_work0);
         if (!write_node(ref, &node_work0, &ref)) return false;
      }
      return rotate_right(ref, out);
   }

   if (bf < -1) {
      if (!read_node(ref, &node_work0, NULL)) return false;
      child = node_work0.m_right;
      if (balance_factor(child) > 0) {
         if (!rotate_right(child, &tmp)) return false;
         if (!read_node(ref, &node_work0, NULL)) return false;
         node_work0.m_right = tmp;
         update_height(&node_work0);
         if (!write_node(ref, &node_work0, &ref)) return false;
      }
      return rotate_left(ref, out);
   }

   if (!read_node(ref, &node_work0, NULL)) return false;
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
static bool data_find_ref_rec(NarfRef ref, const char *key, NarfRef *found, Node *outnode) {
   int cmp;

   while (!ref_is_null(ref)) {
      if (!read_node(ref, &node_work0, NULL)) return false;
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
static bool data_insert_rec(NarfRef rootref, NarfRef itemref, const char *key, NarfRef *out) {
   int cmp;
   NarfRef next;
   NarfRef child;

   if (ref_is_null(rootref)) {
      *out = itemref;
      return true;
   }

   if (!read_node(rootref, &node_work0, NULL)) return false;
   cmp = strcmp(key, node_work0.m_key);
   if (cmp == 0) return false;
   next = (cmp < 0) ? node_work0.m_left : node_work0.m_right;

   if (!data_insert_rec(next, itemref, key, &child)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;

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

//! @brief Delete and return the smallest key in a data AVL subtree.
static bool data_delete_min_rec(NarfRef rootref, NarfRef *out, NarfRef *minref) {
   NarfRef left;
   NarfRef right;
   NarfRef child;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;

   if (ref_is_null(left)) {
      if (minref) *minref = rootref;
      *out = right;
      return true;
   }

   if (!data_delete_min_rec(left, &child, minref)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;
   node_work0.m_left = child;
   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Delete a key from the data AVL tree.
static bool data_delete_rec(NarfRef rootref, const char *key, NarfRef *out, NarfRef *removed_ref, DataPayload *removed_data) {
   NarfRef left;
   NarfRef right;
   NarfRef next;
   NarfRef child;
   NarfRef succref;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;

   cmp = strcmp(key, node_work0.m_key);
   left = node_work0.m_left;
   right = node_work0.m_right;

   if (cmp < 0) {
      next = left;
      if (!data_delete_rec(next, key, &child, removed_ref, removed_data)) return false;
      if (!read_node(rootref, &node_work0, NULL)) return false;
      node_work0.m_left = child;
   }
   else if (cmp > 0) {
      next = right;
      if (!data_delete_rec(next, key, &child, removed_ref, removed_data)) return false;
      if (!read_node(rootref, &node_work0, NULL)) return false;
      node_work0.m_right = child;
   }
   else {
      *removed_ref = rootref;
      if (removed_data) *removed_data = node_work0.m_data;
      if (ref_is_null(left)) {
         *out = right;
         return true;
      }
      if (ref_is_null(right)) {
         *out = left;
         return true;
      }
      if (!data_delete_min_rec(right, &child, &succref)) return false;
      if (!read_node(rootref, &node_work0, NULL)) return false;
      *removed_ref = rootref;
      if (removed_data) *removed_data = node_work0.m_data;
      if (!read_node(succref, &node_work1, NULL)) return false;
      node_work1.m_left = left;
      node_work1.m_right = child;
      update_height(&node_work1);
      if (!write_node(succref, &node_work1, &rootref)) return false;
      return rebalance(rootref, out);
   }

   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Replace the data payload for an existing data-tree key.
static bool data_update_rec(NarfRef rootref, const char *key, const Node *newnode, NarfRef *out) {
   NarfRef next;
   NarfRef child;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;

   cmp = strcmp(key, node_work0.m_key);
   if (cmp < 0) {
      next = node_work0.m_left;
      if (!data_update_rec(next, key, newnode, &child)) return false;
      if (!read_node(rootref, &node_work0, NULL)) return false;
      node_work0.m_left = child;
   }
   else if (cmp > 0) {
      next = node_work0.m_right;
      if (!data_update_rec(next, key, newnode, &child)) return false;
      if (!read_node(rootref, &node_work0, NULL)) return false;
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
static bool free_insert_rec(NarfRef rootref, NarfRef itemref, NarfSector length, NarfSector start, NarfRef *out) {
   NarfRef next;
   NarfRef child;
   int cmp;

   if (ref_is_null(rootref)) {
      *out = itemref;
      return true;
   }

   if (!read_node(rootref, &node_work0, NULL)) return false;
   cmp = free_cmp_values(length, start, itemref.m_sector, &node_work0, rootref.m_sector);
   if (cmp == 0) return false;
   next = (cmp < 0) ? node_work0.m_left : node_work0.m_right;

   if (!free_insert_rec(next, itemref, length, start, &child)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;

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

//! @brief Delete and return the smallest node in a free AVL subtree.
static bool free_delete_min_rec(NarfRef rootref, NarfRef *out, NarfRef *minref) {
   NarfRef left;
   NarfRef right;
   NarfRef child;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;

   if (ref_is_null(left)) {
      if (minref) *minref = rootref;
      *out = right;
      return true;
   }

   if (!free_delete_min_rec(left, &child, minref)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;
   node_work0.m_left = child;
   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Delete a specific extent node from the free AVL tree.
static bool free_delete_rec(NarfRef rootref, NarfSector length, NarfSector start, NarfSector sector, NarfRef *out, NarfRef *removed_ref, FreePayload *removed_free) {
   NarfRef left;
   NarfRef right;
   NarfRef next;
   NarfRef child;
   NarfRef succref;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &node_work0, NULL)) return false;

   cmp = free_cmp_values(length, start, sector, &node_work0, rootref.m_sector);
   left = node_work0.m_left;
   right = node_work0.m_right;

   if (cmp < 0) {
      next = left;
      if (!free_delete_rec(next, length, start, sector, &child, removed_ref, removed_free)) return false;
      if (!read_node(rootref, &node_work0, NULL)) return false;
      node_work0.m_left = child;
   }
   else if (cmp > 0) {
      next = right;
      if (!free_delete_rec(next, length, start, sector, &child, removed_ref, removed_free)) return false;
      if (!read_node(rootref, &node_work0, NULL)) return false;
      node_work0.m_right = child;
   }
   else {
      *removed_ref = rootref;
      if (removed_free) *removed_free = node_work0.m_free;
      if (ref_is_null(left)) {
         *out = right;
         return true;
      }
      if (ref_is_null(right)) {
         *out = left;
         return true;
      }
      if (!free_delete_min_rec(right, &child, &succref)) return false;
      if (!read_node(rootref, &node_work0, NULL)) return false;
      *removed_ref = rootref;
      if (removed_free) *removed_free = node_work0.m_free;
      if (!read_node(succref, &node_work1, NULL)) return false;
      node_work1.m_left = left;
      node_work1.m_right = child;
      update_height(&node_work1);
      if (!write_node(succref, &node_work1, &rootref)) return false;
      return rebalance(rootref, out);
   }

   update_height(&node_work0);
   if (!write_node(rootref, &node_work0, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Find the smallest free extent that can satisfy an allocation.
static bool free_best_rec(NarfRef ref, NarfSector need, NarfRef *bestref, Node *bestnode) {
   bool found = false;

   while (!ref_is_null(ref)) {
      if (!read_node(ref, &node_work0, NULL)) return false;
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

//! @brief Allocate a two-sector record node slot.
static bool alloc_node_sector(NarfRef *ref) {
   NarfRef freeref;
   NarfRef newroot;
   NarfRef removed_ref;
   NarfSector free_start;
   NarfSector free_length;

   if (ref == NULL) return false;

   if (free_best_rec(root.m_free_root, 0, &freeref, &node_work1)) {
      free_start = node_work1.m_free.m_start;
      free_length = node_work1.m_free.m_length;
      if (free_length == 0) {
         if (!free_delete_rec(root.m_free_root, free_length, free_start,
                              freeref.m_sector, &newroot, &removed_ref, NULL)) {
            return false;
         }
         root.m_free_root = newroot;
         *ref = removed_ref;
         return true;
      }
   }

   if (root.m_top < root.m_bottom + 2) return false;
   root.m_top -= 2;
   ref->m_sector = root.m_top;
   ref->m_version = 0;
   return true;
}

//! @brief Insert an already allocated node as a free extent.
static bool insert_free_extent_with_ref(NarfRef ref, NarfSector start, NarfSector length) {
   NarfRef written;
   NarfRef newroot;

   memset(&node_work1, 0, sizeof(node_work1));
   node_work1.m_left = NULL_REF;
   node_work1.m_right = NULL_REF;
   node_work1.m_free.m_start = start;
   node_work1.m_free.m_length = length;
   node_work1.m_height = 1;
   node_work1.m_key[0] = 0;

   if (!write_node(ref, &node_work1, &written)) return false;
   if (!free_insert_rec(root.m_free_root, written, length, start, &newroot)) return false;
   root.m_free_root = newroot;
   return true;
}


//! @brief Set or replace one key-to-index entry in the traversal index tree.
static bool index_set(const char *key, IndexRefs value) {
   NarfRef ref;
   NarfRef written;
   NarfRef newroot;

   if (!valid_key(key)) return false;

   if (data_find_ref_rec(root.m_index_root, key, &ref, &node_work1)) {
      node_work1.m_index = value;
      if (!data_update_rec(root.m_index_root, key, &node_work1, &newroot)) return false;
      root.m_index_root = newroot;
      return true;
   }

   if (!alloc_node_sector(&ref)) return false;

   memset(&node_work1, 0, sizeof(node_work1));
   node_work1.m_left = NULL_REF;
   node_work1.m_right = NULL_REF;
   node_work1.m_height = 1;
   node_work1.m_index = value;
   strcpy(node_work1.m_key, key);

   if (!write_node(ref, &node_work1, &written)) return false;
   if (!data_insert_rec(root.m_index_root, written, key, &newroot)) return false;
   root.m_index_root = newroot;
   return true;
}

//! @brief Delete an index-tree entry when it exists.
static bool index_delete_if_exists(const char *key) {
   NarfRef newroot;
   NarfRef removed_ref;

   if (!valid_key(key)) return false;
   if (ref_is_null(root.m_index_root)) return true;
   if (!data_find_ref_rec(root.m_index_root, key, NULL, NULL)) return true;

   if (!data_delete_rec(root.m_index_root, key, &newroot, &removed_ref, NULL)) return false;
   root.m_index_root = newroot;

   return insert_free_extent_with_ref(removed_ref, END, 0);
}

//! @brief Look up a key in the traversal index tree.
static bool index_get(const char *key, IndexRefs *value) {
   if (value == NULL) return false;
   if (!valid_key(key)) return false;
   if (!data_find_ref_rec(root.m_index_root, key, NULL, &node_work1)) return false;

   *value = node_work1.m_index;
   return true;
}

//! @brief Rebuild the committed traversal index tree from the data tree.
static bool rebuild_indexes_rec(NarfRef ref, NarfRef parent, NarfRef *previous) {
   IndexRefs index;
   IndexRefs prev_index;
   NarfRef left;
   NarfRef right;

   if (ref_is_null(ref)) return true;
   if (!read_node(ref, &node_work0, NULL)) return false;
   left = node_work0.m_left;

   if (!rebuild_indexes_rec(left, ref, previous)) return false;
   if (!read_node(ref, &node_work0, NULL)) return false;
   right = node_work0.m_right;
   strncpy(key_work, node_work0.m_key, sizeof(key_work));
   key_work[sizeof(key_work) - 1] = 0;

   memset(&index, 0, sizeof(index));
   index.m_parent = parent;
   index.m_previous = *previous;
   index.m_next = NULL_REF;

   if (!index_set(key_work, index)) return false;

   if (!ref_is_null(*previous)) {
      if (!read_node(*previous, &node_tmp, NULL)) return false;
      strncpy(key_work, node_tmp.m_key, sizeof(key_work));
      key_work[sizeof(key_work) - 1] = 0;
      if (!index_get(key_work, &prev_index)) return false;
      prev_index.m_next = ref;
      if (!index_set(key_work, prev_index)) return false;
   }

   *previous = ref;

   if (!rebuild_indexes_rec(right, ref, previous)) return false;

   return true;
}

//! @brief Rebuild the committed traversal index tree.
static bool rebuild_indexes(void) {
   NarfRef previous = NULL_REF;

   if (!rebuild_indexes_rec(root.m_data_root, NULL_REF, &previous)) {
      return false;
   }

   return true;
}

//! @brief Create a free-tree node for a free data extent.
static bool insert_free_extent(NarfSector start, NarfSector length) {
   NarfRef ref;
   if (!alloc_node_sector(&ref)) return false;
   return insert_free_extent_with_ref(ref, start, length);
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
   NarfRef freeref;
   NarfRef newroot;
   NarfRef removed_ref;
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
                           freeref.m_sector, &newroot, &removed_ref, &removed_free)) {
         return false;
      }

      root.m_free_root = newroot;
      *start = removed_free.m_start;

      if (removed_free.m_length > length) {
         return insert_free_extent_with_ref(removed_ref,
                                            removed_free.m_start + length,
                                            removed_free.m_length - length);
      }

      return insert_free_extent_with_ref(removed_ref, END, 0);
   }

   if (root.m_top < root.m_bottom) return false;
   if (length > root.m_top - root.m_bottom) return false;

   *start = root.m_bottom;
   root.m_bottom += length;
   return true;
}

//! @brief Allocate a record node and optional payload storage for a new entry.
static bool allocate_storage(NarfSector length, NarfRef *metaref, NarfSector *start) {
   NarfRef freeref;
   NarfRef newroot;
   NarfRef removed_ref;
   FreePayload removed_free;
   NarfSector free_start;
   NarfSector free_length;

   if (length > 0 && free_best_rec(root.m_free_root, length, &freeref, &node_work1)) {
      free_start = node_work1.m_free.m_start;
      free_length = node_work1.m_free.m_length;
      if (!free_delete_rec(root.m_free_root, free_length, free_start, freeref.m_sector,
                           &newroot, &removed_ref, &removed_free)) return false;
      root.m_free_root = newroot;
      *metaref = removed_ref;
      *start = removed_free.m_start;
      if (removed_free.m_length > length) {
         if (!insert_free_extent(removed_free.m_start + length,
                                 removed_free.m_length - length)) return false;
      }
      return true;
   }

   if (root.m_top < root.m_bottom) return false;
   if (length > root.m_top - root.m_bottom) return false;
   if (root.m_top - root.m_bottom - length < 2) return false;
   if (!alloc_node_sector(metaref)) return false;
   *start = root.m_bottom;
   root.m_bottom += length;
   return true;
}

#ifdef NARF_MBR_UTILS
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

   if (!narf_io_open()) return false;
   loval = read_root_copy_version(start, 0, &loversion);
   hival = read_root_copy_version(start, 1, &hiversion);

   if (loval && hival) {
      chosen = version_after(hiversion, loversion) ? 1 : 0;
   }
   else if (loval) {
      chosen = 0;
   }
   else if (hival) {
      chosen = 1;
   }
   else {
      return false;
   }

   if (!read_root_copy(start, chosen, &root_tmp)) return false;
   root_from_disk(&root_tmp);
   root_copy = chosen;

   if (verify()) {
      lfsr_state = root.m_lfsr_seed;
      if (lfsr_state == 0) {
         lfsr_state = 0x1f2e3d4c;
      }
      return true;
   }

   return false;
}

//! @brief Sum positive-length free extents in the free tree.
static bool free_sector_count_rec(NarfRef ref, NarfSector *sectors) {
   NarfRef left;
   NarfRef right;
   NarfSector free_start;
   NarfSector free_length;

   if (sectors == NULL) return false;
   if (ref_is_null(ref)) return true;
   if (!read_node(ref, &node_work0, NULL)) return false;

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

//! @brief Return whether a key exists in the data tree.
bool narf_find(const char *key) {
   return valid_key(key) && verify() && data_find_ref_rec(root.m_data_root, key, NULL, NULL);
}

static const char *dir_prefix(const char *dirname, const char *sep) {
   size_t sep_len = strlen(sep);

   if (sep_len != 0 && !strncmp(dirname, sep, sep_len)) {
      return dirname + sep_len;
   }

   return dirname;
}

static bool dir_match(const char *key, const char *dirname, const char *sep) {
   const char *prefix = dir_prefix(dirname, sep);
   size_t prefix_len = strlen(prefix);
   size_t sep_len = strlen(sep);
   const char *p;

   if (strncmp(prefix, key, prefix_len)) return false;
   p = strstr(key + prefix_len, sep);
   return p == NULL || p[sep_len] == 0;
}

//! @brief Scan the data tree for the next directory entry after a key.
static bool dir_scan_rec(NarfRef ref, const char *dirname, const char *sep, const char *after, const char **best) {
   NarfRef left;
   NarfRef right;
   int cmp_after;

   if (ref_is_null(ref)) return true;
   if (!read_node(ref, &node_work0, NULL)) return false;
   left = node_work0.m_left;

   if (!dir_scan_rec(left, dirname, sep, after, best)) return false;
   if (!read_node(ref, &node_work0, NULL)) return false;
   right = node_work0.m_right;

   cmp_after = (after == NULL) ? 1 : strcmp(node_work0.m_key, after);
   if (cmp_after > 0 && dir_match(node_work0.m_key, dirname, sep)) {
      if (*best == NULL || strcmp(node_work0.m_key, *best) < 0) {
         strncpy(dir_key, node_work0.m_key, sizeof(dir_key));
         dir_key[sizeof(dir_key) - 1] = 0;
         *best = dir_key;
      }
   }

   if (!dir_scan_rec(right, dirname, sep, after, best)) return false;
   return true;
}

const char *narf_dirfirst(const char *dirname, const char *sep) {
   const char *best = NULL;
   if (!verify()) return NULL;
   if (!valid_dir_args(dirname, sep)) return NULL;
   if (!dir_scan_rec(root.m_data_root, dirname, sep, NULL, &best)) return NULL;
   return best;
}

const char *narf_dirnext(const char *dirname, const char *sep, const char *previous_key) {
   const char *best = NULL;
   IndexRefs index;
   NarfRef next;

   if (!verify()) return NULL;
   if (!valid_dir_args(dirname, sep)) return NULL;
   if (!valid_key(previous_key)) return NULL;

   if (index_get(previous_key, &index)) {
      next = index.m_next;
      while (!ref_is_null(next)) {
         if (!read_node(next, &node_work0, NULL)) break;
         strncpy(key_work, node_work0.m_key, sizeof(key_work));
         key_work[sizeof(key_work) - 1] = 0;
         if (dir_match(key_work, dirname, sep)) {
            strncpy(dir_key, key_work, sizeof(dir_key));
            dir_key[sizeof(dir_key) - 1] = 0;
            return dir_key;
         }
         if (!index_get(key_work, &index)) break;
         next = index.m_next;
      }
   }

   if (!dir_scan_rec(root.m_data_root, dirname, sep, previous_key, &best)) return NULL;
   return best;
}

//! @brief Create a key with zero-filled payload storage.
bool narf_alloc(const char *key, NarfByteSize bytes) {
   NarfSector length;
   NarfSector start = END;
   NarfRef metaref;
   NarfRef written;
   NarfRef newroot;

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
   node_work1.m_left = NULL_REF;
   node_work1.m_right = NULL_REF;
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
   if (!rebuild_indexes()) {
      transaction_rollback();
      return false;
   }
   if (!commit_root()) {
      transaction_rollback();
      return false;
   }
   return true;
}

//! @brief Resize an existing key.
bool narf_realloc(const char *key, NarfByteSize bytes) {
   NarfRef newroot;
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

   if (!rebuild_indexes()) {
      transaction_rollback();
      return false;
   }

   if (!commit_root()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Compatibility wrapper around narf_realloc().
bool narf_realloc_key(const char *key, NarfByteSize bytes) {
   return narf_realloc(key, bytes);
}

bool narf_free(const char *key) {
   NarfRef removed_ref;
   NarfRef newroot;
   DataPayload removed_data;
   NarfSector removed_start;
   NarfSector removed_length;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   transaction_begin();
   if (!data_delete_rec(root.m_data_root, key, &newroot, &removed_ref, &removed_data)) {
      transaction_rollback();
      return false;
   }
   root.m_data_root = newroot;
   removed_start = removed_data.m_start;
   removed_length = removed_data.m_length;
   if (!index_delete_if_exists(key)) {
      transaction_rollback();
      return false;
   }
   if (!insert_free_extent_with_ref(removed_ref, removed_start, removed_length)) {
      transaction_rollback();
      return false;
   }
   if (root.m_count) root.m_count--;
   if (!rebuild_indexes()) {
      transaction_rollback();
      return false;
   }
   if (!commit_root()) {
      transaction_rollback();
      return false;
   }
   return true;
}

//! @brief Compatibility wrapper around narf_free().
bool narf_free_key(const char *key) {
   return narf_free(key);
}

bool narf_rename_key(const char *key, const char *newkey) {
   NarfRef removed_ref;
   NarfRef newroot;
   NarfRef written;
   DataPayload renamed_data;

   if (!verify()) return false;
   if (!valid_key(key) || !valid_key(newkey)) return false;
   if (narf_find(newkey)) return false;
   transaction_begin();
   if (!data_delete_rec(root.m_data_root, key, &newroot, &removed_ref, &renamed_data)) {
      transaction_rollback();
      return false;
   }
   root.m_data_root = newroot;
   if (!index_delete_if_exists(key)) {
      transaction_rollback();
      return false;
   }
   memset(&node_work1, 0, sizeof(node_work1));
   node_work1.m_data = renamed_data;
   node_work1.m_left = NULL_REF;
   node_work1.m_right = NULL_REF;
   node_work1.m_height = 1;
   strncpy(node_work1.m_key, newkey, sizeof(node_work1.m_key));
   node_work1.m_key[sizeof(node_work1.m_key) - 1] = 0;
   if (!write_node(removed_ref, &node_work1, &written) ||
       !data_insert_rec(root.m_data_root, written, newkey, &newroot)) {
      transaction_rollback();
      return false;
   }
   root.m_data_root = newroot;
   if (!rebuild_indexes()) {
      transaction_rollback();
      return false;
   }
   if (!commit_root()) {
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
   NarfRef newroot;

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
   if (!rebuild_indexes()) {
      transaction_rollback();
      return false;
   }
   if (!commit_root()) {
      transaction_rollback();
      return false;
   }
   return true;
}

//! @brief Atomically write bytes at an offset in a key payload.
bool narf_write(const char *key, const void *data, NarfByteSize size, NarfByteSize offset) {
   NarfRef newroot;
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

   if (!rebuild_indexes()) {
      transaction_rollback();
      return false;
   }

   if (!commit_root()) {
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
static bool defrag_lowest_free_rec(NarfRef ref, NarfRef *bestref, FreePayload *bestfree) {
   NarfRef left;
   NarfRef right;
   NarfSector free_start;
   NarfSector free_length;
   bool found = false;

   if (bestref == NULL || bestfree == NULL) return false;

   if (ref_is_null(ref)) {
      return false;
   }

   if (!read_node(ref, &node_work0, NULL)) return false;
   left = node_work0.m_left;
   right = node_work0.m_right;
   free_start = node_work0.m_free.m_start;
   free_length = node_work0.m_free.m_length;

   if (defrag_lowest_free_rec(left, bestref, bestfree)) {
      found = true;
   }

   if (free_start != END && free_length != 0) {
      if (!found || free_start < bestfree->m_start) {
         *bestref = ref;
         bestfree->m_start = free_start;
         bestfree->m_length = free_length;
         found = true;
      }
   }

   if (defrag_lowest_free_rec(right, bestref, bestfree)) {
      found = true;
   }

   return found;
}

//! @brief Find a real free payload extent by starting sector.
static bool defrag_find_free_start_rec(NarfRef ref, NarfSector start, NarfRef *found, FreePayload *outfree) {
   NarfRef left;
   NarfRef right;
   NarfSector free_start;
   NarfSector free_length;

   if (ref_is_null(ref)) return false;
   if (!read_node(ref, &node_work0, NULL)) return false;

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
static bool defrag_find_data_start_rec(NarfRef ref, NarfSector start, NarfRef *found,
                                       DataPayload *outdata, char *outkey) {
   NarfRef left;
   NarfRef right;
   NarfSector data_start;
   NarfSector data_length;

   if (ref_is_null(ref)) return false;
   if (!read_node(ref, &node_work0, NULL)) return false;

   left = node_work0.m_left;
   right = node_work0.m_right;
   data_start = node_work0.m_data.m_start;
   data_length = node_work0.m_data.m_length;

   if (data_start == start && data_length != 0) {
      if (found) *found = ref;
      if (outdata) *outdata = node_work0.m_data;
      if (outkey) {
         strncpy(outkey, node_work0.m_key, KEYSIZE);
         outkey[KEYSIZE - 1] = 0;
      }
      return true;
   }

   if (defrag_find_data_start_rec(left, start, found, outdata, outkey)) return true;
   return defrag_find_data_start_rec(right, start, found, outdata, outkey);
}

//! @brief Copy payload sectors from one extent to another.
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
   NarfRef newroot;

   if (!data_find_ref_rec(root.m_data_root, key, NULL, &node_work1)) return false;
   node_work1.m_data.m_start = start;

   if (!data_update_rec(root.m_data_root, key, &node_work1, &newroot)) return false;
   root.m_data_root = newroot;
   return true;
}

//! @brief Carve unused tail sectors from overlong payload extents.
static bool defrag_carve_rec(NarfRef ref, bool *changed) {
   NarfRef left;
   NarfRef right;
   NarfSector data_start;
   NarfSector data_length;
   NarfByteSize data_bytes;
   NarfSector needed;
   NarfSector free_start;
   NarfSector free_length;
   NarfRef newroot;

   if (ref_is_null(ref)) return true;
   if (changed == NULL) return false;
   if (!read_node(ref, &node_work0, NULL)) return false;
   left = node_work0.m_left;

   if (!defrag_carve_rec(left, changed)) return false;
   if (*changed) return true;

   if (!read_node(ref, &node_work0, NULL)) return false;
   right = node_work0.m_right;
   data_start = node_work0.m_data.m_start;
   data_length = node_work0.m_data.m_length;
   data_bytes = node_work0.m_data.m_bytes;
   strncpy(key_work, node_work0.m_key, sizeof(key_work));
   key_work[sizeof(key_work) - 1] = 0;

   needed = BYTES2SECTORS(data_bytes);
   if (needed < data_length) {
      transaction_begin();

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

      if (!rebuild_indexes()) {
         transaction_rollback();
         return false;
      }

      if (!commit_root()) {
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
static bool defrag_merge_free(NarfRef leftref, const FreePayload *left, NarfRef rightref, const FreePayload *right) {
   NarfRef newroot;
   NarfRef removed_ref;
   NarfRef right_removed_ref;
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
   new_length = left_length + right_length;

   if (!free_delete_rec(root.m_free_root, left_length, left_start,
                        leftref.m_sector, &newroot, &removed_ref, NULL)) {
      transaction_rollback();
      return false;
   }
   root.m_free_root = newroot;

   if (!free_delete_rec(root.m_free_root, right_length, right_start,
                        rightref.m_sector, &newroot, &right_removed_ref, NULL)) {
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

   if (!commit_root()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Move an adjacent data extent into or beyond a free extent.
static bool defrag_move_data_after_free(NarfRef freeref, const FreePayload *free_node, NarfRef dataref, const DataPayload *data_node, const char *data_key) {
   NarfRef newroot;
   NarfRef removed_ref;
   NarfSector free_start;
   NarfSector free_length;
   NarfSector old_start;
   NarfSector old_length;
   NarfSector new_start;
   NarfSector new_free_start;
   NarfSector new_free_length;

   (void) dataref;

   if (free_node == NULL || data_node == NULL || data_key == NULL) return false;

   free_start = free_node->m_start;
   free_length = free_node->m_length;
   old_start = data_node->m_start;
   old_length = data_node->m_length;

   if (free_start == END || old_start == END) return false;
   if (free_start + free_length != old_start) return false;

   if (free_length >= old_length) {
      new_start = free_start;
      new_free_start = free_start + old_length;
      new_free_length = free_length;
   }
   else {
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

   if (!free_delete_rec(root.m_free_root, free_length, free_start,
                        freeref.m_sector, &newroot, &removed_ref, NULL)) {
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

   if (!insert_free_extent_with_ref(removed_ref, new_free_start, new_free_length)) {
      transaction_rollback();
      return false;
   }

   if (!rebuild_indexes()) {
      transaction_rollback();
      return false;
   }

   if (!commit_root()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Reclaim a free payload extent that sits at the current data high-water mark.
static bool defrag_lower_bottom(NarfRef freeref, const FreePayload *free_node) {
   NarfRef newroot;
   NarfRef removed_ref;
   NarfSector free_start;
   NarfSector free_length;

   if (free_node == NULL) return false;
   free_start = free_node->m_start;
   free_length = free_node->m_length;

   if (free_start == END) return false;
   if (free_start + free_length != root.m_bottom) return false;

   transaction_begin();

   if (!free_delete_rec(root.m_free_root, free_length, free_start,
                        freeref.m_sector, &newroot, &removed_ref, NULL)) {
      transaction_rollback();
      return false;
   }

   root.m_free_root = newroot;
   root.m_bottom = free_start;

   if (!insert_free_extent_with_ref(removed_ref, END, 0)) {
      transaction_rollback();
      return false;
   }

   if (!commit_root()) {
      transaction_rollback();
      return false;
   }

   return true;
}

//! @brief Perform one power-loss-safe payload squish step.
static bool defrag_squish_once(bool *changed) {
   NarfRef freeref;
   NarfRef adjref;
   FreePayload free_node;
   FreePayload adj_free;
   DataPayload adj_data;
   NarfSector successor;

   if (changed == NULL) return false;
   *changed = false;

   if (!defrag_lowest_free_rec(root.m_free_root, &freeref, &free_node)) {
      return true;
   }

   successor = free_node.m_start + free_node.m_length;

   if (defrag_find_free_start_rec(root.m_free_root, successor, &adjref, &adj_free)) {
      if (!defrag_merge_free(freeref, &free_node, adjref, &adj_free)) return false;
      *changed = true;
      return true;
   }

   if (defrag_find_data_start_rec(root.m_data_root, successor, &adjref, &adj_data, key_work)) {
      if (!defrag_move_data_after_free(freeref, &free_node, adjref, &adj_data, key_work)) return false;
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

//! @brief Reclaim one zero-length record node from the top of the record area.
static bool defrag_tidy_once(bool *changed) {
   NarfRef newroot;
   NarfRef removed_ref;

   if (changed == NULL) return false;
   *changed = false;

   if (!valid_sector_pair(root.m_top)) {
      return true;
   }

   transaction_begin();

   if (!free_delete_rec(root.m_free_root, 0, END, root.m_top,
                        &newroot, &removed_ref, NULL)) {
      transaction_rollback();
      return true;
   }

   root.m_free_root = newroot;
   root.m_top += 2;

   if (!commit_root()) {
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
static void print_tree_node(NarfRef ref, const Node *n, const char *label) {
   if (label[0] == 'I') {
      printf("'%s' [%08x:%08x] I-> P[%08x:%08x] V[%08x:%08x] N[%08x:%08x] h=%u",
             n->m_key, ref.m_sector, ref.m_version,
             n->m_index.m_parent.m_sector, n->m_index.m_parent.m_version,
             n->m_index.m_previous.m_sector, n->m_index.m_previous.m_version,
             n->m_index.m_next.m_sector, n->m_index.m_next.m_version,
             n->m_height);
   }
   else if (label[0] == 'F') {
      printf("'%s' [%08x:%08x] F-> start:len=(%08x:%u) h=%u",
             n->m_key, ref.m_sector, ref.m_version,
             n->m_free.m_start, (unsigned)n->m_free.m_length, n->m_height);
   }
   else {
      printf("'%s' [%08x:%08x] %s-> start:len=(%08x:%u) bytes=%u h=%u",
             n->m_key, ref.m_sector, ref.m_version, label,
             n->m_data.m_start, (unsigned)n->m_data.m_length, (unsigned)n->m_data.m_bytes, n->m_height);
      print_debug_metadata(n->m_data.m_metadata);
   }
}

//! @brief Print one debug tree sideways, using line-drawing limbs when enabled.
static void print_tree(NarfRef ref, int indent, uint64_t pattern, const char *label) {
   NarfRef left = NULL_REF;
   NarfRef right = NULL_REF;
   int i;
   const char *arm;

   if (!ref_is_null(ref)) {
      if (!read_node(ref, &node_work0, NULL)) {
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

   if (!read_node(ref, &node_work0, NULL)) {
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
   printf("root.m_version       = %08x\n", root.m_version);
   printf("root.m_root_version  = %u copy=%d\n", root.m_root_version, root_copy);
   printf("root.m_total_sectors = %08x\n", root.m_total_sectors);
   printf("root.m_data_root     = [%08x:%08x]\n", root.m_data_root.m_sector, root.m_data_root.m_version);
   printf("root.m_free_root     = [%08x:%08x]\n", root.m_free_root.m_sector, root.m_free_root.m_version);
   printf("root.m_index_root    = [%08x:%08x]\n", root.m_index_root.m_sector, root.m_index_root.m_version);
   printf("root.m_lfsr_seed     = %08x\n", root.m_lfsr_seed);
   printf("root.m_count         = %08x\n", root.m_count);
   printf("root.m_bottom        = %08x\n", root.m_bottom);
   printf("root.m_top           = %08x\n", root.m_top);
   printf("data tree:\n");
   print_tree(root.m_data_root, 0, 0, "D");
   printf("free tree:\n");
   print_tree(root.m_free_root, 0, 0, "F");
   printf("index tree:\n");
   print_tree(root.m_index_root, 0, 0, "I");
}
#endif

