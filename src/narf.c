#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#include "narf_conf.h"
#include "narf.h"
#include "narf_io.h"

#ifdef __GNUC__
   #define PACKED __attribute__((packed))
#else
   #define PACKED
#endif

#define SIGNATURE 0x4652414E
#define VERSION 0x00000003
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

typedef struct PACKED {
   NarfSector   m_start;
   NarfSector   m_length;
   NarfByteSize m_bytes;
   uint8_t      m_metadata[128];
} DataPayload;

typedef struct PACKED {
   NarfSector m_start;
   NarfSector m_length;
} FreePayload;

#define NODE_USED (2 * sizeof(NarfRef) + 1 + sizeof(DataPayload) + 4 + 4)

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
   uint32_t     m_node_version;
   uint32_t     m_checksum;
} Node;
static_assert(sizeof(Node) == NARF_SECTOR_SIZE, "Node wrong size");

#define BYTES2SECTORS(x) \
   ((((x) / (NARF_SECTOR_SIZE * 2)) + \
   (((x) % (NARF_SECTOR_SIZE * 2)) != 0)) * 2)
#define KEYSIZE (sizeof(((Node *) 0)->m_key))

static uint8_t buffer[NARF_SECTOR_SIZE];
static Root root;
static int root_copy = 0;
static char dir_key[KEYSIZE];
static uint32_t lfsr_state = 1;

typedef struct {
   bool used;
   NarfSector sector;
   uint32_t version;
   int copy;
} DirtyNode;

#define DIRTY_MAX 4096
static DirtyNode dirty_nodes[DIRTY_MAX];
static int dirty_count = 0;

static void dirty_clear(void);

#ifndef HAVE_ZLIB
//! @brief Compute a fallback CRC-32 checksum when zlib is not available.
uint32_t crc32(uint32_t crc, const void *data, int length) {
   int i, j;
   const uint8_t *p = (const uint8_t *) data;

   for (i = 0; i < length; i++) {
      crc ^= p[i];
      for (j = 0; j < 8; j++) {
         if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
         else crc >>= 1;
      }
   }

   return ~crc;
}
#endif

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
   Node n;

   if (valid_sector_pair(sector)) {
      if (read_node_copy_any(sector, 0, &n)) c0 = n.m_node_version;
      if (read_node_copy_any(sector, 1, &n)) c1 = n.m_node_version;
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

//! @brief Read and validate one of the two root copies.
static bool read_root_copy(NarfSector origin, int which, Root *out) {
   if (!narf_io_read(origin + (NarfSector) which, out)) return false;
   if (out->m_signature != SIGNATURE) return false;
   if (out->m_version != VERSION) return false;
   if (out->m_sector_size != NARF_SECTOR_SIZE) return false;
   if (out->m_checksum != root_checksum(out)) return false;
   return true;
}

//! @brief Commit the current in-memory root as the newest root copy.
static bool commit_root(void) {
   int dest = 1 - root_copy;
   root.m_root_version = root.m_root_version + 1;
   root.m_lfsr_seed = lfsr_next();
   root.m_checksum = 0;
   root.m_checksum = crc32(0, &root, NARF_SECTOR_SIZE - sizeof(uint32_t));
   if (!narf_io_write(root.m_origin + (NarfSector) dest, &root)) return false;
   root_copy = dest;
   dirty_clear();
   return true;
}

//! @brief Initialize an empty root block for a new filesystem.
static bool init_root(NarfSector origin, NarfSector size) {
   Root copy0;
   Root copy1;

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
   root.m_root_version = 1;
   root.m_lfsr_seed = mkfs_lfsr_seed(origin, size);
   lfsr_state = root.m_lfsr_seed;

   copy0 = root;
   copy1 = root;

   copy0.m_root_version = 1;
   copy0.m_checksum = 0;
   copy0.m_checksum = crc32(0, &copy0, NARF_SECTOR_SIZE - sizeof(uint32_t));

   copy1.m_root_version = 0;
   copy1.m_checksum = 0;
   copy1.m_checksum = crc32(0, &copy1, NARF_SECTOR_SIZE - sizeof(uint32_t));

   if (!narf_io_write(origin + 1, &copy1)) return false;
   if (!narf_io_write(origin + 0, &copy0)) return false;

   root = copy0;
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

//! @brief Find a node slot already dirtied by the current transaction.
static int dirty_find(NarfSector sector) {
   int i;
   for (i = 0; i < dirty_count; i++) {
      if (dirty_nodes[i].used && dirty_nodes[i].sector == sector) return i;
   }
   return -1;
}

//! @brief Record which node copy/version is dirty in the current transaction.
static bool dirty_remember(NarfSector sector, uint32_t version, int copy) {
   int i = dirty_find(sector);
   if (i >= 0) {
      dirty_nodes[i].version = version;
      dirty_nodes[i].copy = copy;
      return true;
   }
   if (dirty_count >= DIRTY_MAX) return false;
   dirty_nodes[dirty_count].used = true;
   dirty_nodes[dirty_count].sector = sector;
   dirty_nodes[dirty_count].version = version;
   dirty_nodes[dirty_count].copy = copy;
   dirty_count++;
   return true;
}

//! @brief Clear transaction-local dirty-node tracking.
static void dirty_clear(void) {
   dirty_count = 0;
}

static bool write_node(NarfRef oldref, Node *n, NarfRef *newref) {
   int oldcopy = 1;
   int dest;
   uint32_t oldver = oldref.m_version;
   NarfSector sector = oldref.m_sector;
   Node tmp;
   int dirty;

   if (sector == END) return false;

   dirty = dirty_find(sector);
   if (dirty >= 0) {
      dest = dirty_nodes[dirty].copy;
      n->m_node_version = dirty_nodes[dirty].version;
   }
   else {
      if (oldref.m_version != 0 && read_node(oldref, &tmp, &oldcopy)) {
         dest = 1 - oldcopy;
      }
      else {
         dest = 0;
      }
      n->m_node_version = new_node_version(oldver, sector);
      if (!dirty_remember(sector, n->m_node_version, dest)) return false;
   }

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
   Node n;
   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &n, NULL)) return 0;
   return n.m_height;
}

//! @brief Recompute an AVL node height from its children.
static void update_height(Node *n) {
   int lh = height(n->m_left);
   int rh = height(n->m_right);
   n->m_height = (uint8_t)((lh > rh ? lh : rh) + 1);
}

//! @brief Compute the AVL balance factor for a referenced node.
static int balance_factor(NarfRef ref) {
   Node n;
   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &n, NULL)) return 0;
   return height(n.m_left) - height(n.m_right);
}

//! @brief Perform a copy-on-write AVL right rotation.
static bool rotate_right(NarfRef yref, NarfRef *out) {
   Node y, x;
   NarfRef xref, y2, x2;

   if (!read_node(yref, &y, NULL)) return false;
   xref = y.m_left;
   if (!read_node(xref, &x, NULL)) return false;

   y.m_left = x.m_right;
   update_height(&y);
   if (!write_node(yref, &y, &y2)) return false;

   x.m_right = y2;
   update_height(&x);
   if (!write_node(xref, &x, &x2)) return false;

   *out = x2;
   return true;
}

//! @brief Perform a copy-on-write AVL left rotation.
static bool rotate_left(NarfRef xref, NarfRef *out) {
   Node x, y;
   NarfRef yref, x2, y2;

   if (!read_node(xref, &x, NULL)) return false;
   yref = x.m_right;
   if (!read_node(yref, &y, NULL)) return false;

   x.m_right = y.m_left;
   update_height(&x);
   if (!write_node(xref, &x, &x2)) return false;

   y.m_left = x2;
   update_height(&y);
   if (!write_node(yref, &y, &y2)) return false;

   *out = y2;
   return true;
}

//! @brief Rebalance a copy-on-write AVL subtree.
static bool rebalance(NarfRef ref, NarfRef *out) {
   Node n, child;
   NarfRef tmp;
   int bf;

   if (ref_is_null(ref)) {
      *out = ref;
      return true;
   }

   bf = balance_factor(ref);

   if (bf > 1) {
      if (!read_node(ref, &n, NULL)) return false;
      if (balance_factor(n.m_left) < 0) {
         if (!rotate_left(n.m_left, &tmp)) return false;
         n.m_left = tmp;
         update_height(&n);
         if (!write_node(ref, &n, &ref)) return false;
      }
      return rotate_right(ref, out);
   }

   if (bf < -1) {
      if (!read_node(ref, &n, NULL)) return false;
      if (balance_factor(n.m_right) > 0) {
         if (!rotate_right(n.m_right, &tmp)) return false;
         n.m_right = tmp;
         update_height(&n);
         if (!write_node(ref, &n, &ref)) return false;
      }
      return rotate_left(ref, out);
   }

   if (!read_node(ref, &child, NULL)) return false;
   update_height(&child);
   return write_node(ref, &child, out);
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
   Node n;
   int cmp;

   while (!ref_is_null(ref)) {
      if (!read_node(ref, &n, NULL)) return false;
      cmp = strcmp(key, n.m_key);
      if (cmp == 0) {
         if (found) *found = ref;
         if (outnode) *outnode = n;
         return true;
      }
      ref = (cmp < 0) ? n.m_left : n.m_right;
   }

   return false;
}

//! @brief Insert a node reference into the data AVL tree.
static bool data_insert_rec(NarfRef rootref, NarfRef itemref, const char *key, NarfRef *out) {
   Node n;
   int cmp;
   NarfRef child;

   if (ref_is_null(rootref)) {
      *out = itemref;
      return true;
   }

   if (!read_node(rootref, &n, NULL)) return false;
   cmp = strcmp(key, n.m_key);
   if (cmp == 0) return false;

   if (cmp < 0) {
      if (!data_insert_rec(n.m_left, itemref, key, &child)) return false;
      n.m_left = child;
   }
   else {
      if (!data_insert_rec(n.m_right, itemref, key, &child)) return false;
      n.m_right = child;
   }

   update_height(&n);
   if (!write_node(rootref, &n, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Find the smallest key in a data AVL subtree.
static bool data_min(NarfRef ref, NarfRef *minref, Node *minnode) {
   Node n;
   if (ref_is_null(ref)) return false;
   while (true) {
      if (!read_node(ref, &n, NULL)) return false;
      if (ref_is_null(n.m_left)) {
         *minref = ref;
         *minnode = n;
         return true;
      }
      ref = n.m_left;
   }
}

//! @brief Delete a key from the data AVL tree.
static bool data_delete_rec(NarfRef rootref, const char *key, NarfRef *out, NarfRef *removed_ref, Node *removed_node) {
   Node n, succ;
   NarfRef child, succref;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &n, NULL)) return false;

   cmp = strcmp(key, n.m_key);
   if (cmp < 0) {
      if (!data_delete_rec(n.m_left, key, &child, removed_ref, removed_node)) return false;
      n.m_left = child;
   }
   else if (cmp > 0) {
      if (!data_delete_rec(n.m_right, key, &child, removed_ref, removed_node)) return false;
      n.m_right = child;
   }
   else {
      *removed_ref = rootref;
      *removed_node = n;
      if (ref_is_null(n.m_left)) {
         *out = n.m_right;
         return true;
      }
      if (ref_is_null(n.m_right)) {
         *out = n.m_left;
         return true;
      }
      if (!data_min(n.m_right, &succref, &succ)) return false;
      if (!data_delete_rec(n.m_right, succ.m_key, &child, removed_ref, removed_node)) return false;
      /* data_delete_rec above overwrote removed_* with successor. Restore original removed node. */
      *removed_ref = rootref;
      *removed_node = n;
      succ.m_left = n.m_left;
      succ.m_right = child;
      update_height(&succ);
      if (!write_node(succref, &succ, &rootref)) return false;
      return rebalance(rootref, out);
   }

   update_height(&n);
   if (!write_node(rootref, &n, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Replace the data payload for an existing data-tree key.
static bool data_update_rec(NarfRef rootref, const char *key, const Node *newnode, NarfRef *out) {
   Node n;
   NarfRef child;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &n, NULL)) return false;

   cmp = strcmp(key, n.m_key);
   if (cmp < 0) {
      if (!data_update_rec(n.m_left, key, newnode, &child)) return false;
      n.m_left = child;
   }
   else if (cmp > 0) {
      if (!data_update_rec(n.m_right, key, newnode, &child)) return false;
      n.m_right = child;
   }
   else {
      n = *newnode;
   }

   update_height(&n);
   if (!write_node(rootref, &n, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Insert an extent node into the free AVL tree.
static bool free_insert_rec(NarfRef rootref, NarfRef itemref, NarfSector length, NarfSector start, NarfRef *out) {
   Node n;
   NarfRef child;
   int cmp;

   if (ref_is_null(rootref)) {
      *out = itemref;
      return true;
   }

   if (!read_node(rootref, &n, NULL)) return false;
   cmp = free_cmp_values(length, start, itemref.m_sector, &n, rootref.m_sector);
   if (cmp < 0) {
      if (!free_insert_rec(n.m_left, itemref, length, start, &child)) return false;
      n.m_left = child;
   }
   else if (cmp > 0) {
      if (!free_insert_rec(n.m_right, itemref, length, start, &child)) return false;
      n.m_right = child;
   }
   else {
      return false;
   }

   update_height(&n);
   if (!write_node(rootref, &n, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Find the smallest node in a free AVL subtree.
static bool free_min(NarfRef ref, NarfRef *minref, Node *minnode) {
   Node n;
   if (ref_is_null(ref)) return false;
   while (true) {
      if (!read_node(ref, &n, NULL)) return false;
      if (ref_is_null(n.m_left)) {
         *minref = ref;
         *minnode = n;
         return true;
      }
      ref = n.m_left;
   }
}

//! @brief Delete a specific extent node from the free AVL tree.
static bool free_delete_rec(NarfRef rootref, NarfSector length, NarfSector start, NarfSector sector, NarfRef *out, NarfRef *removed_ref, Node *removed_node) {
   Node n, succ;
   NarfRef child, succref;
   int cmp;

   if (ref_is_null(rootref)) return false;
   if (!read_node(rootref, &n, NULL)) return false;

   cmp = free_cmp_values(length, start, sector, &n, rootref.m_sector);
   if (cmp < 0) {
      if (!free_delete_rec(n.m_left, length, start, sector, &child, removed_ref, removed_node)) return false;
      n.m_left = child;
   }
   else if (cmp > 0) {
      if (!free_delete_rec(n.m_right, length, start, sector, &child, removed_ref, removed_node)) return false;
      n.m_right = child;
   }
   else {
      *removed_ref = rootref;
      *removed_node = n;
      if (ref_is_null(n.m_left)) {
         *out = n.m_right;
         return true;
      }
      if (ref_is_null(n.m_right)) {
         *out = n.m_left;
         return true;
      }
      if (!free_min(n.m_right, &succref, &succ)) return false;
      if (!free_delete_rec(n.m_right, succ.m_free.m_length, succ.m_free.m_start, succref.m_sector, &child, removed_ref, removed_node)) return false;
      *removed_ref = rootref;
      *removed_node = n;
      succ.m_left = n.m_left;
      succ.m_right = child;
      update_height(&succ);
      if (!write_node(succref, &succ, &rootref)) return false;
      return rebalance(rootref, out);
   }

   update_height(&n);
   if (!write_node(rootref, &n, &rootref)) return false;
   return rebalance(rootref, out);
}

//! @brief Find the smallest free extent that can satisfy an allocation.
static bool free_best_rec(NarfRef ref, NarfSector need, NarfRef *bestref, Node *bestnode) {
   Node n;
   bool found = false;

   while (!ref_is_null(ref)) {
      if (!read_node(ref, &n, NULL)) return false;
      if (n.m_free.m_length >= need) {
         *bestref = ref;
         *bestnode = n;
         found = true;
         ref = n.m_left;
      }
      else {
         ref = n.m_right;
      }
   }

   return found;
}

//! @brief Allocate a two-sector record node slot.
static bool alloc_node_sector(NarfRef *ref) {
   NarfRef freeref;
   NarfRef newroot;
   NarfRef removed_ref;
   Node free_node;
   Node removed;

   if (ref == NULL) return false;

   if (free_best_rec(root.m_free_root, 0, &freeref, &free_node) &&
       free_node.m_free.m_length == 0) {
      if (!free_delete_rec(root.m_free_root, free_node.m_free.m_length,
                           free_node.m_free.m_start, freeref.m_sector,
                           &newroot, &removed_ref, &removed)) {
         return false;
      }
      root.m_free_root = newroot;
      *ref = removed_ref;
      return true;
   }

   if (root.m_top < root.m_bottom + 2) return false;
   root.m_top -= 2;
   ref->m_sector = root.m_top;
   ref->m_version = 0;
   return true;
}

//! @brief Insert an already allocated node as a free extent.
static bool insert_free_extent_with_ref(NarfRef ref, NarfSector start, NarfSector length) {
   Node n;
   NarfRef written, newroot;

   memset(&n, 0, sizeof(n));
   n.m_left = NULL_REF;
   n.m_right = NULL_REF;
   n.m_free.m_start = start;
   n.m_free.m_length = length;
   n.m_height = 1;
   n.m_key[0] = 0;

   if (!write_node(ref, &n, &written)) return false;
   if (!free_insert_rec(root.m_free_root, written, length, start, &newroot)) return false;
   root.m_free_root = newroot;
   return true;
}


//! @brief Set or replace one key-to-index entry in the traversal index tree.
static bool index_set(const char *key, IndexRefs value) {
   Node n;
   NarfRef ref;
   NarfRef written;
   NarfRef newroot;

   if (!valid_key(key)) return false;

   if (data_find_ref_rec(root.m_index_root, key, &ref, &n)) {
      n.m_index = value;
      if (!data_update_rec(root.m_index_root, key, &n, &newroot)) return false;
      root.m_index_root = newroot;
      return true;
   }

   if (!alloc_node_sector(&ref)) return false;

   memset(&n, 0, sizeof(n));
   n.m_left = NULL_REF;
   n.m_right = NULL_REF;
   n.m_height = 1;
   n.m_index = value;
   strcpy(n.m_key, key);

   if (!write_node(ref, &n, &written)) return false;
   if (!data_insert_rec(root.m_index_root, written, key, &newroot)) return false;
   root.m_index_root = newroot;
   return true;
}

//! @brief Delete an index-tree entry when it exists.
static bool index_delete_if_exists(const char *key) {
   NarfRef newroot;
   NarfRef removed_ref;
   Node removed;

   if (!valid_key(key)) return false;
   if (ref_is_null(root.m_index_root)) return true;
   if (!data_find_ref_rec(root.m_index_root, key, NULL, NULL)) return true;

   if (!data_delete_rec(root.m_index_root, key, &newroot, &removed_ref, &removed)) return false;
   root.m_index_root = newroot;

   return insert_free_extent_with_ref(removed_ref, END, 0);
}

//! @brief Look up a key in the traversal index tree.
static bool index_get(const char *key, IndexRefs *value) {
   Node n;

   if (value == NULL) return false;
   if (!valid_key(key)) return false;
   if (!data_find_ref_rec(root.m_index_root, key, NULL, &n)) return false;

   *value = n.m_index;
   return true;
}

//! @brief Rebuild the committed traversal index tree from the data tree.
static bool rebuild_indexes_rec(NarfRef ref, NarfRef parent, NarfRef *previous, char *previous_key) {
   Node n;
   IndexRefs index;
   IndexRefs prev_index;

   if (ref_is_null(ref)) return true;
   if (!read_node(ref, &n, NULL)) return false;

   if (!rebuild_indexes_rec(n.m_left, ref, previous, previous_key)) return false;

   memset(&index, 0, sizeof(index));
   index.m_parent = parent;
   index.m_previous = *previous;
   index.m_next = NULL_REF;

   if (!index_set(n.m_key, index)) return false;

   if (!ref_is_null(*previous)) {
      if (!index_get(previous_key, &prev_index)) return false;
      prev_index.m_next = ref;
      if (!index_set(previous_key, prev_index)) return false;
   }

   *previous = ref;
   strcpy(previous_key, n.m_key);

   if (!rebuild_indexes_rec(n.m_right, ref, previous, previous_key)) return false;

   return true;
}

//! @brief Rebuild the committed traversal index tree.
static bool rebuild_indexes(void) {
   NarfRef previous = NULL_REF;
   char previous_key[KEYSIZE];

   previous_key[0] = 0;

   if (!rebuild_indexes_rec(root.m_data_root, NULL_REF, &previous, previous_key)) {
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
   uint8_t zero[NARF_SECTOR_SIZE];
   NarfSector i;

   if (length == 0) return true;
   if (start == END) return false;

   memset(zero, 0, sizeof(zero));

   for (i = 0; i < length; i++) {
      if (!narf_io_write(root.m_origin + start + i, zero)) {
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
   Node free_node;
   Node removed;

   if (start == NULL) return false;

   if (length == 0) {
      *start = END;
      return true;
   }

   if (free_best_rec(root.m_free_root, length, &freeref, &free_node)) {
      if (!free_delete_rec(root.m_free_root, free_node.m_free.m_length,
                           free_node.m_free.m_start, freeref.m_sector,
                           &newroot, &removed_ref, &removed)) {
         return false;
      }

      root.m_free_root = newroot;
      *start = removed.m_free.m_start;

      if (removed.m_free.m_length > length) {
         return insert_free_extent_with_ref(removed_ref,
                                            removed.m_free.m_start + length,
                                            removed.m_free.m_length - length);
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
   NarfRef freeref, newroot;
   Node free_node, removed;
   NarfRef removed_ref;

   if (length > 0 && free_best_rec(root.m_free_root, length, &freeref, &free_node)) {
      if (!free_delete_rec(root.m_free_root, free_node.m_free.m_length, free_node.m_free.m_start, freeref.m_sector,
                           &newroot, &removed_ref, &removed)) return false;
      root.m_free_root = newroot;
      *metaref = removed_ref;
      *start = removed.m_free.m_start;
      if (removed.m_free.m_length > length) {
         if (!insert_free_extent(removed.m_free.m_start + length, removed.m_free.m_length - length)) return false;
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
   Root lo, hi;
   bool loval;
   bool hival;

   if (!narf_io_open()) return false;
   loval = read_root_copy(start, 0, &lo);
   hival = read_root_copy(start, 1, &hi);

   if (loval && hival) {
      if (version_after(hi.m_root_version, lo.m_root_version)) {
         root = hi;
         root_copy = 1;
      }
      else {
         root = lo;
         root_copy = 0;
      }
   }
   else if (loval) {
      root = lo;
      root_copy = 0;
   }
   else if (hival) {
      root = hi;
      root_copy = 1;
   }
   else {
      return false;
   }

   if (verify()) {
      lfsr_state = root.m_lfsr_seed;
      if (lfsr_state == 0) {
         lfsr_state = 0x1f2e3d4c;
      }
      return true;
   }

   return false;
}

//! @brief Return whether a key exists in the data tree.
bool narf_find(const char *key) {
   return valid_key(key) && verify() && data_find_ref_rec(root.m_data_root, key, NULL, NULL);
}

static bool dir_match(const char *key, const char *dirname, const char *sep) {
   size_t dirname_len = strlen(dirname);
   size_t sep_len = strlen(sep);
   const char *p;

   if (strncmp(dirname, key, dirname_len)) return false;
   p = strstr(key + dirname_len, sep);
   return p == NULL || p[sep_len] == 0;
}

//! @brief Scan the data tree for the next directory entry after a key.
static bool dir_scan_rec(NarfRef ref, const char *dirname, const char *sep, const char *after, const char **best) {
   Node n;
   int cmp_after;

   if (ref_is_null(ref)) return true;
   if (!read_node(ref, &n, NULL)) return false;

   if (!dir_scan_rec(n.m_left, dirname, sep, after, best)) return false;

   cmp_after = (after == NULL) ? 1 : strcmp(n.m_key, after);
   if (cmp_after > 0 && dir_match(n.m_key, dirname, sep)) {
      if (*best == NULL || strcmp(n.m_key, *best) < 0) {
         strncpy(dir_key, n.m_key, sizeof(dir_key));
         dir_key[sizeof(dir_key) - 1] = 0;
         *best = dir_key;
      }
   }

   if (!dir_scan_rec(n.m_right, dirname, sep, after, best)) return false;
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
   Node n;

   if (!verify()) return NULL;
   if (!valid_dir_args(dirname, sep)) return NULL;
   if (!valid_key(previous_key)) return NULL;

   if (index_get(previous_key, &index)) {
      next = index.m_next;
      while (!ref_is_null(next)) {
         if (!read_node(next, &n, NULL)) break;
         if (dir_match(n.m_key, dirname, sep)) {
            strncpy(dir_key, n.m_key, sizeof(dir_key));
            dir_key[sizeof(dir_key) - 1] = 0;
            return dir_key;
         }
         if (!index_get(n.m_key, &index)) break;
         next = index.m_next;
      }
   }

   if (!dir_scan_rec(root.m_data_root, dirname, sep, previous_key, &best)) return NULL;
   return best;
}

//! @brief Create a key with zero-filled payload storage.
bool narf_alloc(const char *key, NarfByteSize bytes) {
   Root saved = root;
   NarfSector length;
   NarfSector start = END;
   NarfRef metaref;
   NarfRef written;
   NarfRef newroot;
   Node n;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (narf_find(key)) return false;

   dirty_clear();
   length = BYTES2SECTORS(bytes);
   if (!allocate_storage(length, &metaref, &start)) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!zero_extent(start, length)) {
      root = saved;
      dirty_clear();
      return false;
   }

   memset(&n, 0, sizeof(n));
   n.m_left = NULL_REF;
   n.m_right = NULL_REF;
   n.m_data.m_start = length ? start : END;
   n.m_data.m_length = length;
   n.m_data.m_bytes = bytes;
   n.m_height = 1;
   strcpy(n.m_key, key);

   if (!write_node(metaref, &n, &written) ||
       !data_insert_rec(root.m_data_root, written, key, &newroot)) {
      root = saved;
      dirty_clear();
      return false;
   }

   root.m_data_root = newroot;
   root.m_count++;
   if (!rebuild_indexes()) {
      root = saved;
      dirty_clear();
      return false;
   }
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

//! @brief Resize an existing key.
bool narf_realloc(const char *key, NarfByteSize bytes) {
   Root saved = root;
   NarfRef newroot;
   Node n;
   NarfSector old_start;
   NarfSector old_length;
   NarfSector new_length;
   NarfSector free_start;
   NarfSector free_length;

   if (!verify()) return false;
   if (!valid_key(key)) return false;

   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) {
      return narf_alloc(key, bytes);
   }

   if (bytes > n.m_data.m_bytes) {
      return narf_write(key, NULL, bytes - n.m_data.m_bytes, n.m_data.m_bytes);
   }

   old_start = n.m_data.m_start;
   old_length = n.m_data.m_length;
   new_length = BYTES2SECTORS(bytes);

   dirty_clear();

   if (bytes == 0) {
      if (old_length != 0) {
         if (old_start == END) {
            root = saved;
            dirty_clear();
            return false;
         }

         if (!insert_free_extent(old_start, old_length)) {
            root = saved;
            dirty_clear();
            return false;
         }
      }

      n.m_data.m_start = END;
      n.m_data.m_length = 0;
      n.m_data.m_bytes = 0;
   }
   else if (new_length < old_length) {
      if (old_start == END) {
         root = saved;
         dirty_clear();
         return false;
      }

      free_start = old_start + new_length;
      free_length = old_length - new_length;

      if (free_start < old_start) {
         root = saved;
         dirty_clear();
         return false;
      }

      if (!insert_free_extent(free_start, free_length)) {
         root = saved;
         dirty_clear();
         return false;
      }

      n.m_data.m_length = new_length;
      n.m_data.m_bytes = bytes;
   }
   else {
      n.m_data.m_bytes = bytes;
   }

   if (!data_update_rec(root.m_data_root, key, &n, &newroot)) {
      root = saved;
      dirty_clear();
      return false;
   }

   root.m_data_root = newroot;

   if (!rebuild_indexes()) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }

   return true;
}

//! @brief Compatibility wrapper around narf_realloc().
bool narf_realloc_key(const char *key, NarfByteSize bytes) {
   return narf_realloc(key, bytes);
}

bool narf_free(const char *key) {
   Root saved = root;
   NarfRef removed_ref;
   NarfRef newroot;
   Node removed;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   dirty_clear();
   if (!data_delete_rec(root.m_data_root, key, &newroot, &removed_ref, &removed)) return false;
   root.m_data_root = newroot;
   if (!index_delete_if_exists(key)) {
      root = saved;
      dirty_clear();
      return false;
   }
   if (!insert_free_extent_with_ref(removed_ref, removed.m_data.m_start, removed.m_data.m_length)) {
      root = saved;
      dirty_clear();
      return false;
   }
   if (root.m_count) root.m_count--;
   if (!rebuild_indexes()) {
      root = saved;
      dirty_clear();
      return false;
   }
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

//! @brief Compatibility wrapper around narf_free().
bool narf_free_key(const char *key) {
   return narf_free(key);
}

bool narf_rename_key(const char *key, const char *newkey) {
   Root saved = root;
   NarfRef removed_ref;
   NarfRef newroot;
   NarfRef written;
   Node removed;

   if (!verify()) return false;
   if (!valid_key(key) || !valid_key(newkey)) return false;
   if (narf_find(newkey)) return false;
   dirty_clear();
   if (!data_delete_rec(root.m_data_root, key, &newroot, &removed_ref, &removed)) return false;
   root.m_data_root = newroot;
   if (!index_delete_if_exists(key)) {
      root = saved;
      dirty_clear();
      return false;
   }
   strncpy(removed.m_key, newkey, sizeof(removed.m_key));
   removed.m_key[sizeof(removed.m_key) - 1] = 0;
   removed.m_left = NULL_REF;
   removed.m_right = NULL_REF;
   removed.m_height = 1;
   if (!write_node(removed_ref, &removed, &written) ||
       !data_insert_rec(root.m_data_root, written, newkey, &newroot)) {
      root = saved;
      dirty_clear();
      return false;
   }
   root.m_data_root = newroot;
   if (!rebuild_indexes()) {
      root = saved;
      dirty_clear();
      return false;
   }
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

//! @brief Return the physical sector of a key payload.
NarfSector narf_sector(const char *key) {
   Node n;
   if (!verify()) return END;
   if (!valid_key(key)) return END;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return END;
   if (n.m_data.m_start == END || n.m_data.m_length == 0) return END;
   if (n.m_data.m_start >= root.m_total_sectors) return END;
   if (n.m_data.m_length > root.m_total_sectors - n.m_data.m_start) return END;
   return root.m_origin + n.m_data.m_start;
}

//! @brief Return the byte size of a key payload.
NarfByteSize narf_size(const char *key) {
   Node n;
   if (!verify()) return 0;
   if (!valid_key(key)) return 0;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return 0;
   return n.m_data.m_bytes;
}

void *narf_metadata(const char *key) {
   static uint8_t metadata[128];
   Node n;
   if (!verify()) return NULL;
   if (!valid_key(key)) return NULL;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return NULL;
   memcpy(metadata, n.m_data.m_metadata, sizeof(metadata));
   return metadata;
}

//! @brief Replace a key metadata area.
bool narf_set_metadata(const char *key, void *data) {
   Root saved = root;
   Node n;
   NarfRef newroot;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (data == NULL) return false;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return false;
   dirty_clear();
   memcpy(n.m_data.m_metadata, data, sizeof(n.m_data.m_metadata));
   if (!data_update_rec(root.m_data_root, key, &n, &newroot)) {
      root = saved;
      dirty_clear();
      return false;
   }
   root.m_data_root = newroot;
   if (!rebuild_indexes()) {
      root = saved;
      dirty_clear();
      return false;
   }
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

//! @brief Atomically write bytes at an offset in a key payload.
bool narf_write(const char *key, const void *data, NarfByteSize size, NarfByteSize offset) {
   Root saved = root;
   NarfRef newroot;
   Node n;
   NarfByteSize write_end;
   NarfByteSize new_bytes;
   NarfSector new_length;
   NarfSector new_start;
   NarfSector i;
   const uint8_t *src = (const uint8_t *) data;
   uint8_t temp[NARF_SECTOR_SIZE];
   uint8_t oldbuf[NARF_SECTOR_SIZE];

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (size > ((NarfByteSize) -1) - offset) return false;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return false;

   write_end = offset + size;
   new_bytes = n.m_data.m_bytes;
   if (write_end > new_bytes) new_bytes = write_end;

   if (size == 0 && new_bytes == n.m_data.m_bytes) {
      return true;
   }

   dirty_clear();
   new_length = BYTES2SECTORS(new_bytes);

   if (!allocate_data_extent(new_length, &new_start)) {
      root = saved;
      dirty_clear();
      return false;
   }

   for (i = 0; i < new_length; i++) {
      NarfByteSize base = (NarfByteSize) i * NARF_SECTOR_SIZE;
      NarfByteSize sector_end = base + NARF_SECTOR_SIZE;

      memset(temp, 0, sizeof(temp));

      if (base < n.m_data.m_bytes && n.m_data.m_start != END && i < n.m_data.m_length) {
         NarfByteSize old_n = NARF_SECTOR_SIZE;

         if (sector_end > n.m_data.m_bytes) {
            old_n = n.m_data.m_bytes - base;
         }

         if (!narf_io_read(root.m_origin + n.m_data.m_start + i, oldbuf)) {
            root = saved;
            dirty_clear();
            return false;
         }

         memcpy(temp, oldbuf, old_n);
      }

      if (base < write_end && offset < sector_end) {
         NarfByteSize begin = offset > base ? offset : base;
         NarfByteSize end = write_end < sector_end ? write_end : sector_end;
         NarfByteSize nbytes = end - begin;
         NarfByteSize dest = begin - base;

         if (src != NULL) {
            memcpy(temp + dest, src + (begin - offset), nbytes);
         }
         else {
            memset(temp + dest, 0, nbytes);
         }
      }

      if (!narf_io_write(root.m_origin + new_start + i, temp)) {
         root = saved;
         dirty_clear();
         return false;
      }
   }

   if (n.m_data.m_length > 0) {
      if (!insert_free_extent(n.m_data.m_start, n.m_data.m_length)) {
         root = saved;
         dirty_clear();
         return false;
      }
   }

   n.m_data.m_start = new_length ? new_start : END;
   n.m_data.m_length = new_length;
   n.m_data.m_bytes = new_bytes;

   if (!data_update_rec(root.m_data_root, key, &n, &newroot)) {
      root = saved;
      dirty_clear();
      return false;
   }

   root.m_data_root = newroot;

   if (!rebuild_indexes()) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!commit_root()) {
      root = saved;
      dirty_clear();
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
static bool defrag_lowest_free_rec(NarfRef ref, NarfRef *bestref, Node *bestnode) {
   Node n;
   bool found = false;

   if (bestref == NULL || bestnode == NULL) return false;

   if (ref_is_null(ref)) {
      return false;
   }

   if (!read_node(ref, &n, NULL)) return false;

   if (defrag_lowest_free_rec(n.m_left, bestref, bestnode)) {
      found = true;
   }

   if (n.m_free.m_start != END && n.m_free.m_length != 0) {
      if (!found || n.m_free.m_start < bestnode->m_free.m_start) {
         *bestref = ref;
         *bestnode = n;
         found = true;
      }
   }

   if (defrag_lowest_free_rec(n.m_right, bestref, bestnode)) {
      found = true;
   }

   return found;
}

//! @brief Find a real free payload extent by starting sector.
static bool defrag_find_free_start_rec(NarfRef ref, NarfSector start, NarfRef *found, Node *outnode) {
   Node n;

   if (ref_is_null(ref)) return false;
   if (!read_node(ref, &n, NULL)) return false;

   if (n.m_free.m_start == start && n.m_free.m_length != 0) {
      if (found) *found = ref;
      if (outnode) *outnode = n;
      return true;
   }

   if (defrag_find_free_start_rec(n.m_left, start, found, outnode)) return true;
   return defrag_find_free_start_rec(n.m_right, start, found, outnode);
}

//! @brief Find a data-tree entry by payload starting sector.
static bool defrag_find_data_start_rec(NarfRef ref, NarfSector start, NarfRef *found, Node *outnode) {
   Node n;

   if (ref_is_null(ref)) return false;
   if (!read_node(ref, &n, NULL)) return false;

   if (n.m_data.m_start == start && n.m_data.m_length != 0) {
      if (found) *found = ref;
      if (outnode) *outnode = n;
      return true;
   }

   if (defrag_find_data_start_rec(n.m_left, start, found, outnode)) return true;
   return defrag_find_data_start_rec(n.m_right, start, found, outnode);
}

//! @brief Copy payload sectors from one extent to another.
static bool defrag_copy_extent(NarfSector src, NarfSector dst, NarfSector length) {
   uint8_t tmp[NARF_SECTOR_SIZE];
   NarfSector i;

   if (length == 0 || src == dst) return true;
   if (src == END || dst == END) return false;
   if (src > root.m_total_sectors || length > root.m_total_sectors - src) return false;
   if (dst > root.m_total_sectors || length > root.m_total_sectors - dst) return false;

   for (i = 0; i < length; i++) {
      if (!narf_io_read(root.m_origin + src + i, tmp)) return false;
      if (!narf_io_write(root.m_origin + dst + i, tmp)) return false;
   }

   return true;
}

//! @brief Update one data node after its payload extent has moved.
static bool defrag_update_data_start(const char *key, NarfSector start) {
   NarfRef newroot;
   Node n;

   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return false;
   n.m_data.m_start = start;

   if (!data_update_rec(root.m_data_root, key, &n, &newroot)) return false;
   root.m_data_root = newroot;
   return true;
}

//! @brief Carve unused tail sectors from overlong payload extents.
static bool defrag_carve_rec(NarfRef ref, bool *changed) {
   Root saved;
   Node n;
   NarfSector needed;
   NarfSector free_start;
   NarfSector free_length;
   NarfRef newroot;

   if (ref_is_null(ref)) return true;
   if (changed == NULL) return false;
   if (!read_node(ref, &n, NULL)) return false;

   if (!defrag_carve_rec(n.m_left, changed)) return false;
   if (*changed) return true;

   needed = BYTES2SECTORS(n.m_data.m_bytes);
   if (needed < n.m_data.m_length) {
      saved = root;
      dirty_clear();

      if (needed == 0) {
         free_start = n.m_data.m_start;
         free_length = n.m_data.m_length;
         n.m_data.m_start = END;
         n.m_data.m_length = 0;
      }
      else {
         free_start = n.m_data.m_start + needed;
         free_length = n.m_data.m_length - needed;
         n.m_data.m_length = needed;
      }

      if (free_length != 0 && !insert_free_extent(free_start, free_length)) {
         root = saved;
         dirty_clear();
         return false;
      }

      if (!data_update_rec(root.m_data_root, n.m_key, &n, &newroot)) {
         root = saved;
         dirty_clear();
         return false;
      }
      root.m_data_root = newroot;

      if (!rebuild_indexes()) {
         root = saved;
         dirty_clear();
         return false;
      }

      if (!commit_root()) {
         root = saved;
         dirty_clear();
         return false;
      }

      *changed = true;
      return true;
   }

   return defrag_carve_rec(n.m_right, changed);
}

//! @brief Run one carve pass over the data tree.
static bool defrag_carve_once(bool *changed) {
   if (changed == NULL) return false;
   *changed = false;
   return defrag_carve_rec(root.m_data_root, changed);
}

//! @brief Merge two adjacent free payload extents.
static bool defrag_merge_free(NarfRef leftref, const Node *left, NarfRef rightref, const Node *right) {
   Root saved = root;
   NarfRef newroot;
   NarfRef removed_ref;
   Node removed;
   NarfSector new_length;

   if (left == NULL || right == NULL) return false;
   if (left->m_free.m_start == END || right->m_free.m_start == END) return false;
   if (left->m_free.m_start + left->m_free.m_length != right->m_free.m_start) return false;
   if (right->m_free.m_length > ((NarfSector) -1) - left->m_free.m_length) return false;

   dirty_clear();
   new_length = left->m_free.m_length + right->m_free.m_length;

   if (!free_delete_rec(root.m_free_root, left->m_free.m_length, left->m_free.m_start,
                        leftref.m_sector, &newroot, &removed_ref, &removed)) {
      root = saved;
      dirty_clear();
      return false;
   }
   root.m_free_root = newroot;

   if (!free_delete_rec(root.m_free_root, right->m_free.m_length, right->m_free.m_start,
                        rightref.m_sector, &newroot, &rightref, &removed)) {
      root = saved;
      dirty_clear();
      return false;
   }
   root.m_free_root = newroot;

   if (!insert_free_extent_with_ref(removed_ref, left->m_free.m_start, new_length)) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!insert_free_extent_with_ref(rightref, END, 0)) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }

   return true;
}

//! @brief Move an adjacent data extent into or beyond a free extent.
static bool defrag_move_data_after_free(NarfRef freeref, const Node *free_node, NarfRef dataref, const Node *data_node) {
   Root saved;
   NarfRef newroot;
   NarfRef removed_ref;
   Node removed;
   NarfSector old_start;
   NarfSector old_length;
   NarfSector new_start;
   NarfSector new_free_start;
   NarfSector new_free_length;

   (void) dataref;

   if (free_node == NULL || data_node == NULL) return false;
   if (free_node->m_free.m_start == END || data_node->m_data.m_start == END) return false;
   if (free_node->m_free.m_start + free_node->m_free.m_length != data_node->m_data.m_start) return false;

   old_start = data_node->m_data.m_start;
   old_length = data_node->m_data.m_length;

   if (free_node->m_free.m_length >= old_length) {
      new_start = free_node->m_free.m_start;
      new_free_start = free_node->m_free.m_start + old_length;
      new_free_length = free_node->m_free.m_length;
   }
   else {
      if (root.m_bottom > root.m_top) return false;
      if (old_length > root.m_top - root.m_bottom) return false;
      new_start = root.m_bottom;
      new_free_start = free_node->m_free.m_start;
      new_free_length = free_node->m_free.m_length + old_length;
      if (new_free_length < free_node->m_free.m_length) return false;
   }

   if (!defrag_copy_extent(old_start, new_start, old_length)) {
      return false;
   }

   saved = root;
   dirty_clear();

   if (!free_delete_rec(root.m_free_root, free_node->m_free.m_length, free_node->m_free.m_start,
                        freeref.m_sector, &newroot, &removed_ref, &removed)) {
      root = saved;
      dirty_clear();
      return false;
   }
   root.m_free_root = newroot;

   if (free_node->m_free.m_length < old_length) {
      root.m_bottom += old_length;
   }

   if (!defrag_update_data_start(data_node->m_key, new_start)) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!insert_free_extent_with_ref(removed_ref, new_free_start, new_free_length)) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!rebuild_indexes()) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }

   return true;
}

//! @brief Reclaim a free payload extent that sits at the current data high-water mark.
static bool defrag_lower_bottom(NarfRef freeref, const Node *free_node) {
   Root saved = root;
   NarfRef newroot;
   NarfRef removed_ref;
   Node removed;

   if (free_node == NULL) return false;
   if (free_node->m_free.m_start == END) return false;
   if (free_node->m_free.m_start + free_node->m_free.m_length != root.m_bottom) return false;

   dirty_clear();

   if (!free_delete_rec(root.m_free_root, free_node->m_free.m_length, free_node->m_free.m_start,
                        freeref.m_sector, &newroot, &removed_ref, &removed)) {
      root = saved;
      dirty_clear();
      return false;
   }

   root.m_free_root = newroot;
   root.m_bottom = free_node->m_free.m_start;

   if (!insert_free_extent_with_ref(removed_ref, END, 0)) {
      root = saved;
      dirty_clear();
      return false;
   }

   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }

   return true;
}

//! @brief Perform one power-loss-safe payload squish step.
static bool defrag_squish_once(bool *changed) {
   NarfRef freeref;
   NarfRef adjref;
   Node free_node;
   Node adj;
   NarfSector successor;

   if (changed == NULL) return false;
   *changed = false;

   if (!defrag_lowest_free_rec(root.m_free_root, &freeref, &free_node)) {
      return true;
   }

   successor = free_node.m_free.m_start + free_node.m_free.m_length;

   if (defrag_find_free_start_rec(root.m_free_root, successor, &adjref, &adj)) {
      if (!defrag_merge_free(freeref, &free_node, adjref, &adj)) return false;
      *changed = true;
      return true;
   }

   if (defrag_find_data_start_rec(root.m_data_root, successor, &adjref, &adj)) {
      if (!defrag_move_data_after_free(freeref, &free_node, adjref, &adj)) return false;
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
   Root saved = root;
   NarfRef newroot;
   NarfRef removed_ref;
   Node removed;

   if (changed == NULL) return false;
   *changed = false;

   if (!valid_sector_pair(root.m_top)) {
      return true;
   }

   dirty_clear();

   if (!free_delete_rec(root.m_free_root, 0, END, root.m_top,
                        &newroot, &removed_ref, &removed)) {
      root = saved;
      dirty_clear();
      return true;
   }

   root.m_free_root = newroot;
   root.m_top += 2;

   if (!commit_root()) {
      root = saved;
      dirty_clear();
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
   }
}

//! @brief Print one debug tree sideways, using line-drawing limbs when enabled.
static void print_tree(NarfRef ref, int indent, uint64_t pattern, const char *label) {
   Node n;
   NarfRef left;
   NarfRef right;
   int i;
   const char *arm;

   if (!ref_is_null(ref)) {
      if (!read_node(ref, &n, NULL)) {
         return;
      }

      left = n.m_left;
      right = n.m_right;
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

   printf("%s ", arm);
   print_tree_node(ref, &n, label);
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

