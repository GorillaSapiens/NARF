#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

#ifndef HAVE_LRAND48
#define lrand48 rand
#define srand48 srand
#endif

#define SIGNATURE 0x4652414E
#define VERSION 0x00000002
#define END INVALID_NAF
#define NARF_MIN_FS_SECTORS 4

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
   uint8_t    m_version;
} NarfRef;

#define NULL_REF ((NarfRef){ END, 0 })

#define REF_BYTES (sizeof(NarfRef))
#define ROOT_USED (4 + 4 + sizeof(NarfByteSize) + sizeof(NarfSector) + \
                   2 * sizeof(NarfRef) + 4 * sizeof(NarfSector) + \
                   1 + 4 + 4)

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
   NarfSector   m_count;
   NarfSector   m_bottom;
   NarfSector   m_top;
   NarfSector   m_origin;
   uint8_t      m_root_version;
   uint32_t     m_random;
   uint8_t      m_reserved[NARF_SECTOR_SIZE - ROOT_USED];
   uint32_t     m_checksum;
} Root;
static_assert(sizeof(Root) == NARF_SECTOR_SIZE, "Root wrong size");

#define NODE_USED (2 * sizeof(NarfRef) + 2 * sizeof(NarfSector) + \
                   sizeof(NarfByteSize) + 1 + 128 + 1 + 4 + 4)

typedef struct PACKED {
   NarfRef      m_left;
   NarfRef      m_right;
   NarfSector   m_start;
   NarfSector   m_length;
   NarfByteSize m_bytes;
   uint8_t      m_height;
   uint8_t      m_metadata[128];
   char         m_key[NARF_SECTOR_SIZE - NODE_USED];
   uint8_t      m_node_version;
   uint32_t     m_random;
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

typedef struct {
   bool used;
   NarfSector sector;
   uint8_t version;
   int copy;
} DirtyNode;

#define DIRTY_MAX 4096
static DirtyNode dirty_nodes[DIRTY_MAX];
static int dirty_count = 0;

static void dirty_clear(void);

#ifndef HAVE_ZLIB
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

static bool ref_is_null(NarfRef ref) {
   return ref.m_sector == END || ref.m_version == 0;
}


static bool version_after(uint8_t a, uint8_t b) {
   uint8_t diff = (uint8_t)(a - b);
   return diff != 0 && diff < 128;
}

static uint8_t new_node_version(uint8_t old) {
   uint8_t v;
   do {
      v = (uint8_t)(lrand48() & 0xff);
   } while (v == 0 || v == old);
   return v;
}

static bool verify(void) {
   if (root.m_signature != SIGNATURE) return false;
   if (root.m_version != VERSION) return false;
   if (root.m_sector_size != NARF_SECTOR_SIZE) return false;
   return true;
}

static bool valid_key(const char *key) {
   if (key == NULL) return false;
   if (strlen(key) >= KEYSIZE) return false;
   return true;
}

static bool valid_dir_args(const char *dirname, const char *sep) {
   if (dirname == NULL) return false;
   if (sep == NULL) return false;
   if (sep[0] == 0) return false;
   if (strlen(dirname) >= KEYSIZE) return false;
   if (strlen(sep) >= KEYSIZE) return false;
   return true;
}

static bool valid_sector_pair(NarfSector sector) {
   if (!verify()) return false;
   if (sector == END) return false;
   if (root.m_total_sectors < 2) return false;
   if (sector < root.m_top) return false;
   if (sector > root.m_total_sectors - 2) return false;
   if ((sector & 1) != (root.m_total_sectors & 1)) return false;
   return true;
}

static uint32_t root_checksum(Root *r) {
   uint32_t old = r->m_checksum;
   uint32_t ck;
   r->m_checksum = 0;
   ck = crc32(0, r, NARF_SECTOR_SIZE - sizeof(uint32_t));
   r->m_checksum = old;
   return ck;
}

static bool read_root_copy(NarfSector origin, int which, Root *out) {
   if (!narf_io_read(origin + (NarfSector) which, out)) return false;
   if (out->m_signature != SIGNATURE) return false;
   if (out->m_version != VERSION) return false;
   if (out->m_sector_size != NARF_SECTOR_SIZE) return false;
   if (out->m_checksum != root_checksum(out)) return false;
   return true;
}

static bool commit_root(void) {
   int dest = 1 - root_copy;
   root.m_root_version = (uint8_t)(root.m_root_version + 1);
   root.m_random = (uint32_t) lrand48();
   root.m_checksum = 0;
   root.m_checksum = crc32(0, &root, NARF_SECTOR_SIZE - sizeof(uint32_t));
   if (!narf_io_write(root.m_origin + (NarfSector) dest, &root)) return false;
   root_copy = dest;
   dirty_clear();
   return true;
}

static bool init_root(NarfSector origin, NarfSector size) {
   memset(&root, 0, sizeof(root));
   root.m_signature = SIGNATURE;
   root.m_version = VERSION;
   root.m_sector_size = NARF_SECTOR_SIZE;
   root.m_total_sectors = size;
   root.m_data_root = NULL_REF;
   root.m_free_root = NULL_REF;
   root.m_count = 0;
   root.m_bottom = 2;
   root.m_top = size;
   root.m_origin = origin;
   root.m_root_version = 1;
   root.m_random = (uint32_t) lrand48();
   root.m_checksum = 0;
   root.m_checksum = crc32(0, &root, NARF_SECTOR_SIZE - sizeof(uint32_t));
   root_copy = 0;
   return narf_io_write(origin, &root);
}

static uint32_t node_checksum(Node *n) {
   uint32_t old = n->m_checksum;
   uint32_t ck;
   n->m_checksum = 0;
   ck = crc32(0, n, NARF_SECTOR_SIZE - sizeof(uint32_t));
   n->m_checksum = old;
   return ck;
}

static bool read_node_copy(NarfRef ref, int which, Node *out) {
   if (ref_is_null(ref)) return false;
   if (!valid_sector_pair(ref.m_sector)) return false;
   if (!narf_io_read(root.m_origin + ref.m_sector + (NarfSector) which, out)) return false;
   if (out->m_checksum != node_checksum(out)) return false;
   if (out->m_node_version != ref.m_version) return false;
   return true;
}

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

static int dirty_find(NarfSector sector) {
   int i;
   for (i = 0; i < dirty_count; i++) {
      if (dirty_nodes[i].used && dirty_nodes[i].sector == sector) return i;
   }
   return -1;
}

static bool dirty_remember(NarfSector sector, uint8_t version, int copy) {
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

static void dirty_clear(void) {
   dirty_count = 0;
}

static bool write_node(NarfRef oldref, Node *n, NarfRef *newref) {
   int oldcopy = 1;
   int dest;
   uint8_t oldver = oldref.m_version;
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
      n->m_node_version = new_node_version(oldver);
      if (!dirty_remember(sector, n->m_node_version, dest)) return false;
   }

   n->m_random = (uint32_t) lrand48();
   n->m_checksum = 0;
   n->m_checksum = crc32(0, n, NARF_SECTOR_SIZE - sizeof(uint32_t));

   if (!narf_io_write(root.m_origin + sector + (NarfSector) dest, n)) {
      return false;
   }

   newref->m_sector = sector;
   newref->m_version = n->m_node_version;
   return true;
}

static int height(NarfRef ref) {
   Node n;
   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &n, NULL)) return 0;
   return n.m_height;
}

static void update_height(Node *n) {
   int lh = height(n->m_left);
   int rh = height(n->m_right);
   n->m_height = (uint8_t)((lh > rh ? lh : rh) + 1);
}

static int balance_factor(NarfRef ref) {
   Node n;
   if (ref_is_null(ref)) return 0;
   if (!read_node(ref, &n, NULL)) return 0;
   return height(n.m_left) - height(n.m_right);
}

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

static int free_cmp_values(NarfSector length, NarfSector start, NarfSector sector, const Node *n, NarfSector nsector) {
   if (length < n->m_length) return -1;
   if (length > n->m_length) return 1;
   if (start < n->m_start) return -1;
   if (start > n->m_start) return 1;
   if (sector < nsector) return -1;
   if (sector > nsector) return 1;
   return 0;
}

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
      if (!free_delete_rec(n.m_right, succ.m_length, succ.m_start, succref.m_sector, &child, removed_ref, removed_node)) return false;
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

static bool free_best_rec(NarfRef ref, NarfSector need, NarfRef *bestref, Node *bestnode) {
   Node n;
   bool found = false;

   while (!ref_is_null(ref)) {
      if (!read_node(ref, &n, NULL)) return false;
      if (n.m_length >= need) {
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

static bool alloc_node_sector(NarfRef *ref) {
   if (root.m_top < root.m_bottom + 2) return false;
   root.m_top -= 2;
   ref->m_sector = root.m_top;
   ref->m_version = 0;
   return true;
}

static bool insert_free_extent_with_ref(NarfRef ref, NarfSector start, NarfSector length) {
   Node n;
   NarfRef written, newroot;

   memset(&n, 0, sizeof(n));
   n.m_left = NULL_REF;
   n.m_right = NULL_REF;
   n.m_start = start;
   n.m_length = length;
   n.m_bytes = 0;
   n.m_height = 1;
   n.m_key[0] = 0;

   if (!write_node(ref, &n, &written)) return false;
   if (!free_insert_rec(root.m_free_root, written, length, start, &newroot)) return false;
   root.m_free_root = newroot;
   return true;
}

static bool insert_free_extent(NarfSector start, NarfSector length) {
   NarfRef ref;
   if (!alloc_node_sector(&ref)) return false;
   return insert_free_extent_with_ref(ref, start, length);
}

static bool allocate_storage(NarfSector length, NarfRef *metaref, NarfSector *start) {
   NarfRef freeref, newroot;
   Node free_node, removed;
   NarfRef removed_ref;

   if (length > 0 && free_best_rec(root.m_free_root, length, &freeref, &free_node)) {
      if (!free_delete_rec(root.m_free_root, free_node.m_length, free_node.m_start, freeref.m_sector,
                           &newroot, &removed_ref, &removed)) return false;
      root.m_free_root = newroot;
      *metaref = removed_ref;
      *start = removed.m_start;
      if (removed.m_length > length) {
         if (!insert_free_extent(removed.m_start + length, removed.m_length - length)) return false;
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

bool narf_mkfs(NarfSector start, NarfSector size) {
   if (!narf_io_open()) return false;
   if (size < NARF_MIN_FS_SECTORS) return false;
   if (start > narf_io_sectors()) return false;
   if (size > narf_io_sectors() - start) return false;
   return init_root(start, size);
}

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

   return verify();
}

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
   if (!verify()) return NULL;
   if (!valid_dir_args(dirname, sep)) return NULL;
   if (!valid_key(previous_key)) return NULL;
   if (!dir_scan_rec(root.m_data_root, dirname, sep, previous_key, &best)) return NULL;
   return best;
}

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

   memset(&n, 0, sizeof(n));
   n.m_left = NULL_REF;
   n.m_right = NULL_REF;
   n.m_start = length ? start : END;
   n.m_length = length;
   n.m_bytes = bytes;
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
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

bool narf_realloc(const char *key, NarfByteSize bytes) {
   Root saved = root;
   NarfRef ref;
   NarfRef newroot;
   Node n;
   NarfSector new_length;
   NarfRef freeref;
   NarfSector new_start;
   uint8_t copybuf[NARF_SECTOR_SIZE];
   NarfSector i;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (!data_find_ref_rec(root.m_data_root, key, &ref, &n)) {
      return narf_alloc(key, bytes);
   }

   dirty_clear();
   new_length = BYTES2SECTORS(bytes);
   if (new_length <= n.m_length) {
      n.m_bytes = bytes;
      if (!data_update_rec(root.m_data_root, key, &n, &newroot)) {
         root = saved;
         return false;
      }
      root.m_data_root = newroot;
      if (!commit_root()) {
         root = saved;
         return false;
      }
      return true;
   }

   if (!allocate_storage(new_length, &freeref, &new_start)) {
      root = saved;
      dirty_clear();
      return false;
   }

   for (i = 0; i < n.m_length; i++) {
      if (!narf_io_read(root.m_origin + n.m_start + i, copybuf) ||
          !narf_io_write(root.m_origin + new_start + i, copybuf)) {
         root = saved;
         return false;
      }
   }

   if (n.m_length > 0) {
      if (!insert_free_extent(n.m_start, n.m_length)) {
         root = saved;
         return false;
      }
   }

   n.m_start = new_start;
   n.m_length = new_length;
   n.m_bytes = bytes;
   (void) freeref;

   if (!data_update_rec(root.m_data_root, key, &n, &newroot)) {
      root = saved;
      dirty_clear();
      return false;
   }
   root.m_data_root = newroot;
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

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
   if (!insert_free_extent_with_ref(removed_ref, removed.m_start, removed.m_length)) {
      root = saved;
      dirty_clear();
      return false;
   }
   if (root.m_count) root.m_count--;
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

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
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

NarfSector narf_sector(const char *key) {
   Node n;
   if (!verify()) return END;
   if (!valid_key(key)) return END;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return END;
   if (n.m_start == END || n.m_length == 0) return END;
   if (n.m_start >= root.m_total_sectors) return END;
   if (n.m_length > root.m_total_sectors - n.m_start) return END;
   return root.m_origin + n.m_start;
}

NarfByteSize narf_size(const char *key) {
   Node n;
   if (!verify()) return 0;
   if (!valid_key(key)) return 0;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return 0;
   return n.m_bytes;
}

void *narf_metadata(const char *key) {
   static uint8_t metadata[128];
   Node n;
   if (!verify()) return NULL;
   if (!valid_key(key)) return NULL;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return NULL;
   memcpy(metadata, n.m_metadata, sizeof(metadata));
   return metadata;
}

bool narf_set_metadata(const char *key, void *data) {
   Root saved = root;
   Node n;
   NarfRef newroot;

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (data == NULL) return false;
   if (!data_find_ref_rec(root.m_data_root, key, NULL, &n)) return false;
   dirty_clear();
   memcpy(n.m_metadata, data, sizeof(n.m_metadata));
   if (!data_update_rec(root.m_data_root, key, &n, &newroot)) {
      root = saved;
      dirty_clear();
      return false;
   }
   root.m_data_root = newroot;
   if (!commit_root()) {
      root = saved;
      dirty_clear();
      return false;
   }
   return true;
}

bool narf_append(const char *key, const void *data, NarfByteSize size) {
   NarfByteSize old_size;
   NarfSector sector;
   NarfByteSize offset;
   NarfByteSize remain;
   uint8_t temp[NARF_SECTOR_SIZE];

   if (!verify()) return false;
   if (!valid_key(key)) return false;
   if (data == NULL && size != 0) return false;
   old_size = narf_size(key);
   if (!narf_find(key)) return false;
   if (size > ((NarfByteSize)-1) - old_size) return false;
   if (!narf_realloc(key, old_size + size)) return false;

   sector = narf_sector(key);
   if (sector == END && size != 0) return false;
   offset = old_size;
   remain = size;
   while (remain) {
      NarfSector cur = sector + offset / NARF_SECTOR_SIZE;
      NarfByteSize off = offset % NARF_SECTOR_SIZE;
      NarfByteSize n = NARF_SECTOR_SIZE - off;
      if (n > remain) n = remain;
      if (!narf_io_read(cur, temp)) return false;
      memcpy(temp + off, data, n);
      if (!narf_io_write(cur, temp)) return false;
      data = (const uint8_t *) data + n;
      offset += n;
      remain -= n;
   }
   return true;
}

bool narf_append_key(const char *key, const void *data, NarfByteSize size) {
   return narf_append(key, data, size);
}

#ifdef NARF_USE_DEFRAG
bool narf_defrag(void) {
   return verify();
}
#endif

#ifdef NARF_DEBUG
static void print_tree(NarfRef ref, int indent, const char *label) {
   Node n;
   int i;
   if (ref_is_null(ref)) return;
   if (!read_node(ref, &n, NULL)) return;
   print_tree(n.m_left, indent + 1, label);
   for (i = 0; i < indent; i++) printf("   ");
   printf("%s [%08x:%02x] '%s' start:len=(%08x:%u) bytes=%u h=%u\n",
          label, ref.m_sector, ref.m_version, n.m_key,
          n.m_start, (unsigned)n.m_length, (unsigned)n.m_bytes, n.m_height);
   print_tree(n.m_right, indent + 1, label);
}

void narf_debug(void) {
   printf("root.m_signature     = %08x '%.4s'\n", root.m_signature, root.m_sigbytes);
   printf("root.m_version       = %08x\n", root.m_version);
   printf("root.m_root_version  = %u copy=%d\n", root.m_root_version, root_copy);
   printf("root.m_total_sectors = %08x\n", root.m_total_sectors);
   printf("root.m_data_root     = [%08x:%02x]\n", root.m_data_root.m_sector, root.m_data_root.m_version);
   printf("root.m_free_root     = [%08x:%02x]\n", root.m_free_root.m_sector, root.m_free_root.m_version);
   printf("root.m_count         = %08x\n", root.m_count);
   printf("root.m_bottom        = %08x\n", root.m_bottom);
   printf("root.m_top           = %08x\n", root.m_top);
   printf("data tree:\n");
   print_tree(root.m_data_root, 0, "D");
   printf("free tree:\n");
   print_tree(root.m_free_root, 0, "F");
}
#endif

