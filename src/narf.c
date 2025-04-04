#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define HAVE_ZLIB

#ifdef HAVE_ZLIB
#include <zlib.h>   // for crc32
#endif

#include "narf_conf.h"
#include "narf.h"
#include "narf_io.h"

// Uncomment this for debugging functions
#define NARF_DEBUG

// Uncomment this for debugging structure integrity
// Beware, this makes EVERYTHING very slow!
#define NARF_DEBUG_INTEGRITY

// Uncomment for unicode line drawing characters in debug functions
#define USE_UTF8_LINE_DRAWING

#ifdef NARF_DEBUG
   #include <assert.h>
   #include <stdio.h>
   #include <math.h>
#else
   #if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
      #define static_assert(x,y) _Static_assert(x,y)
   #else
      #define static_assert(x,y)
   #endif
   #define assert(x)
#endif

#ifdef __GNUC__
   #define PACKED __attribute__((packed))
#else
   #define PACKED
#endif

#define SIGNATURE 0x4652414E // FRAN => NARF
#define VERSION 0x00000001

#define END INVALID_NAF // i hate typing

#ifndef HAVE_LRAND48
#define lrand48 rand
#define srand48 srand
#endif

///////////////////////////////////////////////////////
//! @brief Classic MBR Partition Entry (16 bytes)
typedef struct PACKED {
   uint8_t  boot_indicator;       // 0x80 if bootable, 0x00 if not
   uint8_t  start_head;           // Start head
   uint8_t  start_sector;         // Start sector (low bits)
   uint8_t  start_cylinder;       // Start cylinder (high bits)
   uint8_t  partition_type;       // Partition type (e.g., 0x07 for NTFS)
   uint8_t  end_head;             // End head
   uint8_t  end_sector;           // End sector (low bits)
   uint8_t  end_cylinder;         // End cylinder (high bits)
   uint32_t start_lba;            // Start of the partition (LBA)
   uint32_t partition_size;       // Size of the partition (LBA)
} MBRPartitionEntry;
static_assert(sizeof(MBRPartitionEntry) == 16, "MBRPartitionEntry wrong size");

#define NARF_PART_TYPE 0x6E // lowercase 'n' (uppercase is taken)

///////////////////////////////////////////////////////
//! @brief Classic MBR sector (512 bytes)
typedef struct PACKED {
   uint8_t boot_code[446];       // Boot code (446 bytes)
   MBRPartitionEntry partitions[4]; // 4 partition entries (16 bytes each)
   uint16_t signature;           // Boot signature (0xAA55)
} MBR;
static_assert(sizeof(MBR) == 512, "MBRPartitionEntry wrong size");
#define MBR_SIGNATURE 0xAA55

///////////////////////////////////////////////////////
//! @brief The Root structure for our Not A Real Filesystem
//!
//! it is kept in memory, and flushed out with narf_end().
//! it is intentionally small.
typedef struct PACKED {
   union {
      uint32_t m_signature;  // SIGNATURE
      uint8_t  m_sigbytes[4];
   };
   uint32_t m_version;       // VERSION

   NarfByteSize m_sector_size;   // sector size in bytes
   NarfSector   m_total_sectors; // total size of storage in sectors

   NAF m_root;               // sector of root node
   NAF m_first;              // sector of first node in key order
   NAF m_last;               // sector of last node in key order
   NAF m_chain;              // previously allocated but free now
   NarfSector   m_count;     // count of total number of allocated NAF
   NarfSector   m_bottom;    // relative number of first unallocated sector
   NarfSector   m_top;       // relative number of last unallocated sector + 1

   NarfSector   m_origin;    // absolute number where the root sector lives

#if 0
   // TODO FIX this is wasted space, fix it.
   uint8_t      m_reserved[ NARF_SECTOR_SIZE -
                            1 * sizeof(NarfByteSize) -
                            5 * sizeof(NarfSector) -
                            4 * sizeof(NAF) -
                            5 * sizeof(uint32_t) ];
#endif

   int32_t      m_generation;  // the generation number
   uint32_t     m_random;      // lfsr sequence number
   uint32_t     m_checksum;    // crc32
} Root;
static_assert(5 * sizeof(uint32_t) +
              1 * sizeof(NarfByteSize) +
              5 * sizeof(NarfSector) +
              4 * sizeof(NAF) == sizeof(Root), "Root wrong size");

///////////////////////////////////////////////////////
//! @brief A Node structure to hold NAF details
typedef struct PACKED {
   NAF m_parent;      // parent NAF
   NAF m_left;        // left sibling NAF
   NAF m_right;       // right sibling NAF
   NAF m_prev;        // previous ordered NAF
   NAF m_next;        // next ordered NAF

   NarfSector   m_start;       // data start sector
   NarfSector   m_length;      // data length in sectors
   NarfByteSize m_bytes;       // data size in bytes

   uint8_t m_height; // used by AVL logic

   uint8_t m_metadata[128]; // not used by NARF

   char         m_key     [ NARF_SECTOR_SIZE -
                            5 * sizeof(NAF) -
                            2 * sizeof(NarfSector) -
                            1 * sizeof(NarfByteSize) -
                            1 -
                            128 -
                            3 * sizeof(uint32_t) ];

   int32_t      m_generation;  // the generation number
   uint32_t     m_random;      // lfsr sequence number
   uint32_t     m_checksum;    // crc32
} Node;
static_assert(sizeof(Node) == NARF_SECTOR_SIZE, "Node wrong size");

///////////////////////////////////////////////////////
//! @brief sectors needed for bytes
//!
//! NB: MUST return a multiple of 2 !!!
#define BYTES2SECTORS(x) \
   (((x) + (NARF_SECTOR_SIZE * 2 - 1)) / (NARF_SECTOR_SIZE * 2)) * 2;

///////////////////////////////////////////////////////
//! @brief bytes in a key?
#define KEYSIZE (sizeof(((Node *) 0)->m_key))

static uint8_t buffer[NARF_SECTOR_SIZE] = { 0 };

static bool write_to_hi = false;
static bool write_root_to_hi;

static Root root = { 0 };
static Node * const node = (Node *) buffer;

#ifndef HAVE_ZLIB
///////////////////////////////////////////////////////
//! @brief crc32 checksum
uint32_t crc32(uint32_t crc, const void *data, int length) {
   int i, j;
   for (i = 0; i < length; i++) {
      crc ^= *((uint8_t *)data); // XOR with input byte
      data = ((uint8_t *) data) + 1;
      for (j = 0; j < 8; j++) { // Process 8 bits
         if (crc & 1) {
            crc = (crc >> 1) ^ 0xEDB88320; // Polynomial for CRC-32
         }
         else {
            crc >>= 1;
         }
      }
   }
   return ~crc; // Final XOR
}
#endif

///////////////////////////////////////////////////////
//! @brief Read a NAF into our buffer
//!
//! @param naf The naf to read
//! @return true on success
static bool read_buffer(NAF naf) {
   int32_t tmp;
   int32_t gen_lo;
   int32_t gen_hi;
   uint32_t ck_lo;
   uint32_t ck_hi;
   bool lo_match;
   bool hi_match;

   assert(naf != END);

   // we don't want to get confused by failed reads
   // and stale data.  we also want to increase entropy.
   // so we load the buffers with random data before
   // we do a read.  this will frustrate crc32 check
   // later.
   node->m_random = lrand48();
   if (!narf_io_read(root.m_origin + naf, buffer)) return false;
   ck_lo = crc32(0, buffer, NARF_SECTOR_SIZE - sizeof(uint32_t));
   lo_match = (ck_lo == node->m_checksum);
   gen_lo = node->m_generation;

   node->m_random = lrand48();
   if (!narf_io_read(root.m_origin + naf + 1, buffer)) return false;
   ck_hi = crc32(0, buffer, NARF_SECTOR_SIZE - sizeof(uint32_t));
   hi_match = (ck_hi == node->m_checksum);
   gen_hi = node->m_generation;

   if (lo_match) {
      if (hi_match) {

         assert(gen_lo == 0 || root.m_generation - gen_lo >= 0);
         assert(gen_hi == 0 || root.m_generation - gen_hi >= 0);

         // both checksums good, we can trust the data

         if ((root.m_generation - gen_lo) >= 0) {
            if ((root.m_generation - gen_hi) >= 0) {
               // both good

               tmp = gen_lo - gen_hi;
               if (tmp > 0) {
                  // lo is more recent
                  goto lo_is_good;
               }
               else if (tmp < 0) {
                  // hi is more recent
                  goto hi_is_good;
               }
               else {
                  // they are the same.

                  // this should not happen.
                  // hrm....
                  assert(0);

                  // random time.
                  if (lrand48() & 1) {
                     goto lo_is_good;
                  }
                  else {
                     goto hi_is_good;
                  }
               }
            }
            else {
               // only lo good
               goto lo_is_good;
            }
         }
         else {
            if ((root.m_generation - gen_hi) >= 0) {
               // only hi good
               goto hi_is_good;
            }
            else {
               // this should never happen
               assert(0);
               return false;
            }
         }
      }
      else {
         // only lo is good
lo_is_good:
         // hi is in the buffer, so we need to re-read lo
         narf_io_read(root.m_origin + naf, buffer);
         write_to_hi = (node->m_generation != root.m_generation);
      }
   }
   else {
      if (hi_match) {
         // only hi is good
hi_is_good:
         // hi is already in the buffer
         write_to_hi = (node->m_generation == root.m_generation);
      }
      else {
         // this should never happen
         assert(0);
         return false;
      }
   }

   return true;
}

///////////////////////////////////////////////////////
//! @brief Write a NAF from buffer to disk
//!
//! @param naf The NAF to write
//! @return true on success
static bool write_buffer(NAF naf) {
   node->m_generation = root.m_generation;
   node->m_random = lrand48();
   node->m_checksum = crc32(0, (void *) node, NARF_SECTOR_SIZE - sizeof(uint32_t));

   return narf_io_write(root.m_origin + naf + (write_to_hi ? 1 : 0), buffer);
}

#ifdef NARF_MBR_UTILS

///////////////////////////////////////////////////////
// derived from bootloader.{asm|bin}
static const uint8_t boot_code_stub[] = {
   0xeb, 0x00, 0xb8, 0xc0, 0x07, 0x8e, 0xd8, 0x8e,
   0xc0, 0xbe, 0x21, 0x7c, 0xe8, 0x02, 0x00, 0xeb,
   0xfe, 0xac, 0x08, 0xc0, 0x74, 0x05, 0xe8, 0x03,
   0x00, 0xeb, 0xf6, 0xc3, 0xb4, 0x0e, 0xcd, 0x10,
   0xc3 };
static const char boot_code_msg[] =
   "NARF! not bootable.\r\n";

///////////////////////////////////////////////////////
//! @brief Write a new blank MBR to the media
//! @see narf_partition
//! @see narf_format
//! @see narf_findpart
//! @see narf_mount
//!
//! VERY DESTRUCTIVE !!!
//!
//! existing MBR is overwritten and blanked
//!
//! @param message Custom boot_code message, or NULL for default
//! @return true for success
bool narf_mbr(const char *message) {
   MBR *mbr = (MBR *) buffer;

   if (!narf_io_open()) return false;

   memset(buffer, 0, sizeof(buffer));
   memcpy(buffer, boot_code_stub, sizeof(boot_code_stub));
   if (message == NULL) {
      message = boot_code_msg;
   }
   strcpy((char *)(buffer + sizeof(boot_code_stub)), message);
   mbr->signature = MBR_SIGNATURE;
   narf_io_write(0, buffer);

   return true;
}

///////////////////////////////////////////////////////
//! @brief Write a new partition table entry to the media
//! @see narf_mbr
//! @see narf_format
//! @see narf_findpart
//! @see narf_mount
//!
//! DESTRUCTIVE !!!
//!
//! existing partition data is overwritten.
//! all available space is used for the new partition.
//!
//! @param partition The partition number (1-4) to occupy
//! @return true for success
bool narf_partition(int partition) {
   int i;
   NarfSector start;
   NarfSector end;
   MBR *mbr;

   if (!narf_io_open()) return false;

   start = 1;
   end = narf_io_sectors();
   mbr = (MBR *) buffer;
   narf_io_read(0, buffer);

   if (partition < 1 || partition > 4) {
      return false;
   }
   --partition;

   // TODO FIX we're making a lot of assumptions here.
   // do we REALLY know partitions are ordered?
   // is this just more complexity than we need?

   for (i = 0; i < 4; i++) {
      if (i < partition) {
         if (mbr->partitions[i].partition_type) {
            start =
               mbr->partitions[i].start_lba +
               mbr->partitions[i].partition_size;
         }
      }
      else if (i > partition) {
         if (mbr->partitions[i].partition_type) {
            end = mbr->partitions[i].start_lba - 1;
            break;
         }
      }
   }

   mbr->partitions[partition].partition_type = NARF_PART_TYPE;
   mbr->partitions[partition].start_lba = start;
   mbr->partitions[partition].partition_size = end - start;

   narf_io_write(0, buffer);

   return true;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_format(int partition) {
   MBR *mbr;

   if (!narf_io_open()) return false;

   mbr = (MBR *) buffer;
   narf_io_read(0, buffer);

   if (partition < 1 || partition > 4) {
#ifdef NARF_DEBUG
      printf("bad partition\n");
#endif
      return false;
   }
   --partition;

   if (mbr->partitions[partition].partition_type != NARF_PART_TYPE) {
#ifdef NARF_DEBUG
      printf("bad type\n");
#endif
      return false;
   }

   return
      narf_mkfs(mbr->partitions[partition].start_lba,
         mbr->partitions[partition].partition_size);
}

///////////////////////////////////////////////////////
//! @brief Find a NARF partition
//! @see narf_mbr
//! @see narf_partition
//! @see narf_format
//! @see narf_findpart
//! @see narf_mount
//!
//! @return A number (1-4) of the partition containint NARF, or -1
int narf_findpart(void) {
   int i;
   MBR *mbr;

   if (!narf_io_open()) return -1;

   mbr = (MBR *) buffer;
   narf_io_read(0, buffer);

   for (i = 0; i < 4; i++) {
      if (mbr->partitions[i].partition_type == NARF_PART_TYPE) {
         return i + 1;
      }
   }

   return 0;
}

///////////////////////////////////////////////////////
//! @brief Mount a NARF partition
//! @see narf_mbr
//! @see narf_partition
//! @see narf_format
//! @see narf_findpart
//! @see narf_mount
//! @see narf_init
//!
//! calls narf_init with correct parameters based on partition
//! table.
//!
//! @param partition The partition (1-4) to mount
//! @return true for success
bool narf_mount(int partition) {
   MBR *mbr;

   if (!narf_io_open()) return false;

   mbr = (MBR *) buffer;
   narf_io_read(0, buffer);

   --partition;

   if (mbr->partitions[partition].partition_type != NARF_PART_TYPE) {
      return false;
   }

   return narf_init(mbr->partitions[partition].start_lba);
}
#endif

#if 0
///////////////////////////////////////////////////////
//! @brief Get the ideal height for our tree.
static int max_height(void) {
   NarfSector i = root.m_count;
   int ret = 1;

   while (i) {
      ++ret;
      i >>= 1;
   }

   return ret;
}
#endif

///////////////////////////////////////////////////////
//! @brief Verify we're working with a valid filesystem
//!
//! determines if init() or mkfs() was called and filesystem is valid
//!
//! @return true on success
static bool verify(void) {
   if (root.m_signature != SIGNATURE) return false;
   if (root.m_version != VERSION) return false;
   if (root.m_sector_size != NARF_SECTOR_SIZE) return false;
   return true;
}

#ifdef NARF_DEBUG

#ifdef USE_UTF8_LINE_DRAWING
#define HORIZ "━"
#define VERT  "┃"
#define UPPER "┏"
#define LOWER "┗"
#define NIL   "❌"
#endif

///////////////////////////////////////////////////////
//! @brief Helper used to pretty print the NARF tree.
//!
//! @param naf The current NAF being printed
//! @param indent The number of levels to indent
//! @param pattern Bitfield indicating tree limbs to print
static void narf_pt(NAF naf, int indent, uint32_t pattern) {
   NAF left;
   NAF right;
   int height;
   NAF start;
   NarfSector length;
   int i;
   char *key;
   char *arm;

   if (!verify()) return;

   if (naf != END) {
      read_buffer(naf);
      left = node->m_left;
      right = node->m_right;
      height = node->m_height;
      start = node->m_start;
      length = node->m_length;
      key = strdup(node->m_key);
      narf_pt(left, indent + 1, pattern);
   }

   for (i = 0; i < indent; i++) {
      if (pattern & (1 << i)) {
#ifndef USE_UTF8_LINE_DRAWING
         printf("|  ");
#else
         printf(VERT "  ");
#endif
      }
      else {
         printf("   ");
      }
   }

   if (indent) {
      if (pattern & (1 << indent)) {
#ifndef USE_UTF8_LINE_DRAWING
         arm = "\\-";
#else
         arm = LOWER HORIZ;
#endif
      }
      else {
#ifndef USE_UTF8_LINE_DRAWING
         arm = "/-";
#else
         arm = UPPER HORIZ;
#endif
      }
   }
   else {
#ifndef USE_UTF8_LINE_DRAWING
      arm = "==";
#else
      arm = HORIZ HORIZ;
#endif
   }

   if (naf == END) {
#ifndef USE_UTF8_LINE_DRAWING
      printf("%s (nil)\n", arm);
#else
      printf("%s%s\n", arm, NIL);
#endif
      return;
   }
   else {
      printf("%s %s [%08x] (%08x:%d) ^%d",
         arm, key, naf, start, length, height);
      if (naf == root.m_first) {
         printf(" (first)");
      }
      if (naf == root.m_last) {
         printf(" (last)");
      }
      if (naf == root.m_root) {
         printf(" (root)");
      }
      if (naf == root.m_chain) {
         // seeing this means there's a serious problem
         printf(" (chain)");
      }
      printf("\n");
      free(key);
   }

   narf_pt(right, indent + 1, (pattern ^ (3 << (indent))) & ~1);
}

///////////////////////////////////////////////////////
//! @see narf_debug
static void print_node(NAF naf) {
   read_buffer(naf);
   printf("naf = [%08x] => '%.*s'\n",
         naf, (int) KEYSIZE, node->m_key);
   printf("tree u/l/r  = [%08x] / [%08x] / [%08x]\n",
         node->m_parent, node->m_left, node->m_right);
   printf("list p/n    = [%08x] / [%08x]\n",
         node->m_prev, node->m_next);
   printf("start:len   = (%08x:%d) (%d)\n",
         node->m_start, node->m_length, node->m_bytes);
   printf("metadata    = '%.*s'\n",
         (int) sizeof(node->m_metadata), node->m_metadata);
   printf("g-r-c       = %d-%08x-%08x\n",
         node->m_generation, node->m_random, node->m_checksum);
}

///////////////////////////////////////////////////////
//! @see narf_debug()
static void print_chain(void) {
   NAF naf;
   if (root.m_chain == END) {
      printf("freechain is empty\n");
   }
   else {
      printf("freechain:\n");
      naf = root.m_chain;
      while (naf != END) {
         read_buffer(naf);
         printf("%08x (%08x:%d) -> %08x\n", naf, node->m_start, node->m_length, node->m_next);
         naf = node->m_next;
      }
      printf("\n");
   }
}

///////////////////////////////////////////////////////
//! @see narf.h
void narf_debug(NAF naf) {
   printf("root.m_signature     = %08x '%.4s'\n", root.m_signature, root.m_sigbytes);
   if (root.m_signature != SIGNATURE) {
      printf("bad signature\n");
      return;
   }

   printf("root.m_version       = %08x\n", root.m_version);
   if (root.m_version != VERSION) {
      printf("bad version\n");
      return;
   }

   printf("root.m_sector_size   = %d\n", root.m_sector_size);
   if (root.m_sector_size != NARF_SECTOR_SIZE) {
      printf("bad sector size\n");
      return;
   }

   printf("root.m_total_sectors = 0x%08x (%10d)\n", root.m_total_sectors, root.m_total_sectors);
   if (root.m_total_sectors < 2) {
      printf("bad total sectors\n");
      return;
   }

   printf("root.m_bottom        = 0x%08x (%10d)\n", root.m_bottom, root.m_bottom);
   printf("root.m_top           = 0x%08x (%10d)\n", root.m_top, root.m_top);
   printf("root.m_chain         = 0x%08x (%10d)\n", root.m_chain, root.m_chain);
   printf("root.m_root          = 0x%08x (%10d)\n", root.m_root, root.m_root);
   printf("root.m_first         = 0x%08x (%10d)\n", root.m_first, root.m_first);
   printf("root.m_last          = 0x%08x (%10d)\n", root.m_last, root.m_last);
   printf("root.m_count         = %d\n", root.m_count);
   printf("root.m_origin        = 0x%08x (%10d)\n", root.m_origin, root.m_origin);
   printf("root.g-r-c           = %d-%08x-%08x\n",
         root.m_generation, root.m_random, root.m_checksum);
   printf("\n");

   if (naf != END) {
      print_node(naf);
      printf("\n");
   }
   else {
      naf = root.m_first;
      while (naf != END) {
         print_node(naf);
         printf("\n");

         naf = node->m_next;
      }
   }

   print_chain();
   printf("\n");

   if (naf == END) {
      if (root.m_root == END) {
         printf("tree is empty\n");
      }
      else {
         printf("tree:\n");
         narf_pt(root.m_root, 0, 0);
      }
   }
}
#endif

#ifdef NARF_DEBUG_INTEGRITY
///////////////////////////////////////////////////////
static void verify_not_on_tree(NAF parent, NAF naf) {
   NAF l;
   NAF r;

   if (parent == END) {
      return;
   }
   assert(parent != naf);

   read_buffer(parent);
   l = node->m_left;
   r = node->m_right;

   verify_not_on_tree(l, naf);
   verify_not_on_tree(r, naf);
}

///////////////////////////////////////////////////////
static void verify_not_in_chain(NAF naf) {
   NAF tmp;
   tmp = root.m_chain;
   while (tmp != END) {
      read_buffer(tmp);
      assert(naf != tmp);
      tmp = node->m_next;
   }
}

///////////////////////////////////////////////////////
//! @brief if it's in the tree, it should not be in chain
static void walk_tree(NAF parent, NAF naf) {
   NAF l;
   NAF r;

   if (naf == END) {
      return;
   }

   verify_not_in_chain(naf);

   // assert parent linkage
   if (parent == END) {
      assert(root.m_root == naf);
   }
   else {
      read_buffer(parent);
      assert((node->m_left == naf || node->m_right == naf) &&
            (node->m_left != node->m_right));
   }

   read_buffer(naf);
   l = node->m_left;
   r = node->m_right;

   // assert parent
   assert(node->m_parent == parent);

   walk_tree(naf, l);
   walk_tree(naf, r);
}

///////////////////////////////////////////////////////
//! @brief if it's in the chain, it should not be in tree
static void walk_chain(void) {
   NAF tmp;
   NAF next;
   tmp = root.m_chain;
   while (tmp != END) {
      read_buffer(tmp);
      next = node->m_next;
      verify_not_on_tree(root.m_root, tmp);
      tmp = next;
   }
}

///////////////////////////////////////////////////////
//! @brief detect a loop in the chain linked list
static void chain_loop(void) {
   NAF a;
   NAF b;

   a = b = root.m_chain;

   while (a != END && b != END) {
      read_buffer(a);
      a = node->m_next;

      read_buffer(b);
      b = node->m_next;
      if (b != END) {
         read_buffer(b);
         b = node->m_next;
      }

      if (a != END && b != END) {
         assert (a != b);
      }
   }
}

///////////////////////////////////////////////////////
//! @brief detect a loop in the ordered linked list
static void order_loop(void) {
   NAF a;
   NAF b;

   a = b = root.m_first;

   while (a != END && b != END) {
      read_buffer(a);
      a = node->m_next;

      read_buffer(b);
      b = node->m_next;
      if (b != END) {
         read_buffer(b);
         b = node->m_next;
      }

      if (a != END && b != END) {
         assert (a != b);
      }
   }
}

///////////////////////////////////////////////////////
//! @brief detect a loop in the reverse order linked list
static void reverse_loop(void) {
   NAF a;
   NAF b;

   a = b = root.m_last;

   while (a != END && b != END) {
      read_buffer(a);
      a = node->m_prev;

      read_buffer(b);
      b = node->m_prev;
      if (b != END) {
         read_buffer(b);
         b = node->m_prev;
      }

      if (a != END && b != END) {
         assert (a != b);
      }
   }
}

///////////////////////////////////////////////////////
static void walk_order(void) {
   NAF prev;
   NAF next;
   NAF naf;
   int count;

   if (root.m_first == END && root.m_last == END) {
      return;
   }

   assert(root.m_first != END && root.m_last != END);

   // walk forward
   count = 0;
   prev = END;
   naf = root.m_first;

   while (naf != END) {
      count++;
      read_buffer(naf);
      next = node->m_next;

      if (node->m_prev == END) {
         assert(root.m_first == naf);
      }
      else {
         read_buffer(node->m_prev);
         assert(node->m_next == naf);
      }

      prev = naf;
      naf = next;
   }

   assert(count == root.m_count);

   // walk backward
   count = 0;
   next = END;
   naf = root.m_last;

   while (naf != END) {
      count++;
      read_buffer(naf);
      prev = node->m_prev;

      if (node->m_next == END) {
         assert(root.m_last == naf);
      }
      else {
         read_buffer(node->m_next);
         assert(node->m_prev == naf);
      }

      next = naf;
      naf = prev;
   }

   assert(count == root.m_count);
}

///////////////////////////////////////////////////////
void walk_space(void) {
   NAF inner, outer;
   NarfSector i_s, i_l, o_s;
   bool based = false;
   bool flag;

   if (root.m_top == root.m_total_sectors) {
      // nothing to check
      return;
   }

   for (inner = root.m_top; inner < root.m_total_sectors; inner += 2) {
      flag = false;
      read_buffer(inner);
      i_s = node->m_start;
      i_l = node->m_length;

      if (i_s == 2) {
         based = true;
      }

      if (i_s + i_l == root.m_bottom) {
         flag = true;
      }
      else if (i_s == END) {
         flag = true;
      }
      else {
         for (outer = root.m_top; outer < root.m_total_sectors; outer += 2) {
            if (inner != outer) {
               read_buffer(outer);
               o_s = node->m_start;

               if (i_s + i_l == o_s) {
                  flag = true;
               }
            }
         }
      }

      assert(flag);
   }
   assert(based);
}

///////////////////////////////////////////////////////
void walk_double_gen(void) {
   NarfSector ns;
   int32_t gen_lo;
   uint32_t ck_lo;
   Root *asroot = (Root *) buffer;

   narf_io_read(root.m_origin + 0, buffer);
   gen_lo = asroot->m_generation;
   ck_lo = asroot->m_checksum;

   narf_io_read(root.m_origin + 1, buffer);
   if (ck_lo != 0 && asroot->m_checksum != 0);
   assert(gen_lo != asroot->m_generation);

   for (ns = root.m_top; ns < root.m_total_sectors; ns++) {
      narf_io_read(ns, buffer);
      gen_lo = asroot->m_generation;
      ck_lo = asroot->m_checksum;
      narf_io_read(ns + 1, buffer);
      //printf("%d %d\n", gen_lo, asroot->m_generation);
      assert(ck_lo == 0 ||
             asroot->m_checksum == 0 ||
             gen_lo != asroot->m_generation);
   }
}

///////////////////////////////////////////////////////
static void verify_integrity(void) {
   order_loop();
   reverse_loop();
   chain_loop();
   walk_chain();
   walk_tree(END, root.m_root);
   walk_order();
   walk_space();
   walk_double_gen();
   // TODO test tree <-> first/last cohesion
}
#endif

///////////////////////////////////////////////////////
//! @brief add a naf to the free chain
//!
//! @param naf The NAF to add
static void narf_chain(NAF naf) {
   NAF prev;
   NAF next;
   NarfSector length; // used in multiple places

   // chain is in order of increasing length

   // can we reclaim some bottom space?
   read_buffer(naf);
   length = node->m_length; // this is from the naf
   if (node->m_start + length == root.m_bottom) {
      root.m_bottom -= length;
      node->m_start = END;
      length = node->m_length = 0;
      write_buffer(naf);
   }

   // can we reclaim some top space?
   if (length == 0 && naf == root.m_top) {
      // rare case where we can just drop it on the floor
      root.m_top += 2;
      return;
   }

   // get the first node length if possible
   if (root.m_chain != END) {
      read_buffer(root.m_chain);
      length = node->m_length; // this is from first node in chain now
   }

   // some special cases go straight to the front

   read_buffer(naf);
   if (root.m_chain == END || node->m_length <= length) {
      node->m_next = root.m_chain;
      root.m_chain = naf;
      write_buffer(naf);
      return;
   }

   length = node->m_length; // this is from naf now

   prev = END;
   next = root.m_chain;
   while(1) {
      if (next != END) {
         read_buffer(next);
      }
      if (next == END || node->m_length >= length) {
         read_buffer(naf);
         node->m_parent = END;
         node->m_left = END;
         node->m_right = END;
         node->m_prev = END;
         node->m_next = next;
         write_buffer(naf);

         read_buffer(prev);
         node->m_next = naf;
         write_buffer(prev);

         return;
      }
      prev = next;
      next = node->m_next;
   }

   assert(0); // we should never get this far
}

///////////////////////////////////////////////////////
static void avl_adjust_heights(NAF naf) {
   int lh, rh, nh;
   NAF left, right;

   while (naf != END) {
      read_buffer(naf);
      left = node->m_left;
      right = node->m_right;

      if (left != END) {
         read_buffer(left);
         lh = node->m_height + 1;
      }
      else {
         lh = 0;
      }

      if (right != END) {
         read_buffer(right);
         rh = node->m_height + 1;
      }
      else {
         rh = 0;
      }

      nh = ((lh > rh) ? lh : rh);

      read_buffer(naf);
      if (node->m_height != nh) {
         node->m_height = nh;
         write_buffer(naf);
      }
      naf = node->m_parent;
   }
}

///////////////////////////////////////////////////////
static int avl_bf(NAF naf) {
   NAF left, right;
   int lh;
   int rh;

   read_buffer(naf);
   left = node->m_left;
   right = node->m_right;

   if (left != END) {
      read_buffer(left);
      lh = node->m_height;
   }
   else {
      lh = -1;
   }

   if (right != END) {
      read_buffer(right);
      rh = node->m_height;
   }
   else {
      rh = -1;
   }

   return (lh - rh);
}

///////////////////////////////////////////////////////
static void avl_ll(NAF naf) { // AKA right rotation
   NAF left, parent, child;

   read_buffer(naf);
   left = node->m_left;
   parent = node->m_parent;

   read_buffer(left);
   child = node->m_right;

   read_buffer(naf);
   node->m_left = child;
   node->m_parent = left;
   write_buffer(naf);

   read_buffer(left);
   node->m_right = naf;
   node->m_parent = parent;
   write_buffer(left);

   if (child != END) {
      read_buffer(child);
      node->m_parent = naf;
      write_buffer(child);
   }

   if (parent != END) {
      read_buffer(parent);
      if (node->m_left == naf) {
         node->m_left = left;
      }
      else if (node->m_right == naf) {
         node->m_right = left;
      }
      else {
         assert(0);
      }
      write_buffer(parent);
   }
   else {
      root.m_root = left;
   }

   avl_adjust_heights(naf);
}

///////////////////////////////////////////////////////
static void avl_rr(NAF naf) { // AKA left rotation
   NAF right, parent, child;

   read_buffer(naf);
   right = node->m_right;
   parent = node->m_parent;

   read_buffer(right);
   child = node->m_left;

   read_buffer(naf);
   node->m_right = child;
   node->m_parent = right;
   write_buffer(naf);

   read_buffer(right);
   node->m_left = naf;
   node->m_parent = parent;
   write_buffer(right);

   if (child != END) {
      read_buffer(child);
      node->m_parent = naf;
      write_buffer(child);
   }

   if (parent != END) {
      read_buffer(parent);
      if (node->m_left == naf) {
         node->m_left = right;
      }
      else if (node->m_right == naf) {
         node->m_right = right;
      }
      else {
         assert(0);
      }
      write_buffer(parent);
   }
   else {
      root.m_root = right;
   }

   avl_adjust_heights(naf);
}

///////////////////////////////////////////////////////
static void avl_lr(NAF naf) {
   // left rotation on the left child
   read_buffer(naf);
   avl_rr(node->m_left);
   // then a right rotation
   avl_ll(naf);
}

///////////////////////////////////////////////////////
static void avl_rl(NAF naf) {
   // right rotation on the right child
   read_buffer(naf);
   avl_ll(node->m_right);
   // then a left rotation
   avl_rr(naf);
}

///////////////////////////////////////////////////////
static void avl_rebalance(NAF naf) {
   int bf, cf;
   NAF left, right;

   while (naf != END) {
      read_buffer(naf);
      left = node->m_left;
      right = node->m_right;

      bf = avl_bf(naf);

      if (bf < -1) {
         // right heavy
         cf = avl_bf(right);
         if (cf < 0) {
            // RR rotation
            avl_rr(naf);
         }
         else {
            // RL rotation
            avl_rl(naf);
         }
      }
      else if (bf > 1) {
         // left heavy
         cf = avl_bf(left);
         if (cf < 0) {
            // LR rotiation
            avl_lr(naf);
         }
         else {
            // LL rotation
            avl_ll(naf);
         }
      }

      read_buffer(naf);
      naf = node->m_parent;
   }
}

///////////////////////////////////////////////////////
//! @brief Insert NAF into the tree and list.
//!
//! @return true for success
static bool narf_insert(NAF naf, const char *key) {
   NAF tmp;
   NAF parent;
   int cmp;
   int height;

   height = 0;

   if (!verify()) return false;

   if (root.m_root == END) {
      root.m_root = naf;
      root.m_first = naf;
      root.m_last = naf;
      read_buffer(naf);
      node->m_height = 0;
      write_buffer(naf);
   }
   else {
      parent = root.m_root;
      while (1) {
         read_buffer(parent);
         cmp = strncmp(key, node->m_key, KEYSIZE);
         if (cmp < 0) {
            if (node->m_left != END) {
               parent = node->m_left;
               ++height;
            }
            else {
               // we are the new left of parent
               node->m_left = naf;
               tmp = node->m_prev;
               node->m_prev = naf;
               write_buffer(parent);

               read_buffer(naf);
               node->m_height = -1;
               node->m_parent = parent;
               node->m_prev = tmp;
               node->m_next = parent;
               write_buffer(naf);

               if (tmp != END) {
                  read_buffer(tmp);
                  node->m_next = naf;
                  write_buffer(tmp);
               }
               else {
                  root.m_first = naf;
               }

               avl_adjust_heights(naf);

               break;
            }
         }
         else if (cmp > 0) {
            if (node->m_right != END) {
               parent = node->m_right;
               ++height;
            }
            else {
               // we are the new right of p
               node->m_right = naf;
               tmp = node->m_next;
               node->m_next = naf;
               write_buffer(parent);

               read_buffer(naf);
               node->m_height = -1;
               node->m_parent = parent;
               node->m_next = tmp;
               node->m_prev = parent;
               write_buffer(naf);

               if (tmp != END) {
                  read_buffer(tmp);
                  node->m_prev = naf;
                  write_buffer(tmp);
               }
               else {
                  root.m_last = naf;
               }

               avl_adjust_heights(naf);

               break;
            }
         }
         else {
            // this should never happen !!!
            assert(0);
         }
      }

      avl_rebalance(naf);
   }
   return true;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_mkfs(NarfSector start, NarfSector size) {
   if (!narf_io_open()) return false;

   memset(buffer, 0, NARF_SECTOR_SIZE);

   root.m_signature     = SIGNATURE;
   root.m_version       = VERSION;
   root.m_sector_size   = NARF_SECTOR_SIZE;
   root.m_total_sectors = size;
   root.m_bottom        = 2;
   root.m_top           = size;
   root.m_root          = END;
   root.m_first         = END;
   root.m_last          = END;
   root.m_chain         = END;
   root.m_count         = 0;
   root.m_origin         = start;

   root.m_generation    = 0;
   root.m_random        = lrand48();
   root.m_checksum      = crc32(0, (void *) &root, sizeof(Root) - sizeof(uint32_t));

   memcpy(buffer, &root, sizeof(root));
   narf_io_write(start, buffer);

   root.m_generation    = 1;
   root.m_random        = lrand48();
   root.m_checksum      = crc32(0, (void *) &root, sizeof(Root) - sizeof(uint32_t));

   memcpy(buffer, &root, sizeof(root));
   narf_io_write(start + 1, buffer);

#ifdef NARF_DEBUG
   printf("keysize %ld\n", KEYSIZE);
#endif

   return true;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_init(NarfSector start) {
   Root *asroot = (Root *) buffer;
   bool ret;
   int32_t tmp;
   int32_t gen_lo;
   int32_t gen_hi;
   uint32_t ck_lo;
   uint32_t ck_hi;
   bool lo_ok;
   bool hi_ok;

#ifdef NARF_DEBUG
   printf("keysize %ld\n", KEYSIZE);
#endif

   if (!narf_io_open()) return false;

   ret = narf_io_read(start, buffer);
   if (!ret) return false;
   ck_lo = crc32(0, (void *) asroot, sizeof(Root) - sizeof(uint32_t));
   lo_ok = (ck_lo == asroot->m_checksum);
   gen_lo = asroot->m_generation;

   ret = narf_io_read(start + 1, buffer);
   if (!ret) return false;
   ck_hi = crc32(0, (void *) asroot, sizeof(Root) - sizeof(uint32_t));
   hi_ok = (ck_hi == asroot->m_checksum);
   gen_hi = asroot->m_generation;

   if (lo_ok) {
      if (hi_ok) {
         // both good, compare generations

         tmp = gen_lo - gen_hi;
         if (tmp > 0) {
            goto lo_is_good;
         }
         else if (tmp < 0) {
            goto hi_is_good;
         }
         else {
            if (lrand48() & 1) {
               goto lo_is_good;
            }
            else {
               goto hi_is_good;
            }
         }
      }
      else {
         // lo good
lo_is_good:
         // hi is still in the buffer, we need to re-read lo
         narf_io_read(start, buffer);
         memcpy(&root, asroot, sizeof(Root));
         write_root_to_hi = true;
      }
   }
   else {
      if (ck_hi == asroot->m_checksum) {
         // hi good
hi_is_good:
         // hi is still in the buffer
         memcpy(&root, asroot, sizeof(Root));
         write_root_to_hi = false;
      }
      else {
         // none good

         // we can't do anything in this case
         assert(0);
         return false;
      }
   }

   srand48(asroot->m_random);

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

   return verify();
}

static int semaphore = 0;

///////////////////////////////////////////////////////
//! @brief begin a transaction
static void narf_begin(void) {
   static bool sanitized = false;
   NarfSector ns;

   if (!sanitized) {
      // kill anything left over from past failed power loss writes

      // we do this here to avoid the cost of doing it at mount time

      // it only needs to be done once per power cycle

      // TODO FIX this is expensive, how can we optimize it?

      for (ns = root.m_top; ns < root.m_total_sectors; ++ns) {
         narf_io_read(root.m_origin + ns, buffer);
         // no need to do checksum verification here
         // because a node we want to keep will not have
         // a higher generation number.
         if ((node->m_generation - root.m_generation) > 0) {
            memset(buffer, 0, sizeof(buffer));
            narf_io_write(root.m_origin + ns, buffer);
         }
      }

      sanitized = true;
   }

   if (!semaphore) {
      ++root.m_generation;
   }
   ++semaphore;
}

///////////////////////////////////////////////////////
//! @brief roll back a transaction
static void narf_rollback(void) {
   --semaphore;
   if (!semaphore) {
      --root.m_generation;
   }
}

///////////////////////////////////////////////////////
//! @brief end a transaction
static void narf_end(void) {
   --semaphore;
   if (!semaphore) {
      root.m_random = lrand48();
      root.m_checksum = crc32(0, (void *) &root, sizeof(Root) - sizeof(uint32_t));
      write_root_to_hi = !write_root_to_hi;

      memset(buffer, 0, sizeof(buffer));
      memcpy(buffer, &root, sizeof(root));
      narf_io_write(root.m_origin + (write_root_to_hi ? 0 : 1), buffer);

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

   }
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_find(const char *key) {
   NAF naf = root.m_root;
   int cmp;

   if (!verify()) return END;

   while(1) {
      if (naf == END) {
         return naf;
      }
      read_buffer(naf);
      cmp = strncmp(key, node->m_key, KEYSIZE);
      if (cmp < 0) {
         naf = node->m_left;
      }
      else if (cmp > 0) {
         naf = node->m_right;
      }
      else {
         return naf;
      }
   }
   // TODO FIX detect endless loops???
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_dirfirst(const char *dirname, const char *sep) {
   NAF naf;
   int cmp;

   if (!verify()) return END;
   if (root.m_root == END) return END;

   if (dirname && dirname[0]) {
      naf = root.m_root;

      while(1) {
         read_buffer(naf);
         cmp = strncmp(dirname, node->m_key, KEYSIZE);
         if (cmp < 0) {
            if (node->m_left != END) {
               naf = node->m_left;
            }
            else {
               // the current node comes AFTER us
               if (node->m_prev != END) {
                  naf = node->m_prev;
               }
               else {
                  naf = END;
               }
               break;
            }
         }
         else if (cmp > 0) {
            if (node->m_right != END) {
               naf = node->m_right;
            }
            else {
               // the current node comes BEFORE us
               // awesome
               break;
            }
         }
         else {
            // BAZINGA !!!
            // an exact match
            return naf;
         }
      }
   }
   else {
      naf = END;
   }

   return narf_dirnext(dirname, sep, naf);
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_dirnext(const char *dirname, const char *sep, NAF naf) {
   uint32_t dirname_len;
   uint32_t sep_len;
   char *p;

   if (!verify()) return END;

   if (naf != END) {
      read_buffer(naf);
      naf = node->m_next;
   }
   else {
      naf = root.m_first;
   }

   if (naf == END) {
      return END;
   }

   read_buffer(naf);

   // at this point, "naf" is (probably) valid,
   // it is in the buffer,
   // and it is the first node after us.

   dirname_len = strlen(dirname);
   if (strncmp(dirname, node->m_key, dirname_len)) {
      // not a match at all!
      return END;
   }

   sep_len = strlen(sep);
   while (!strncmp(dirname, node->m_key, dirname_len)) {
      // beginning matches
      p = strstr(node->m_key + dirname_len, sep);
      if (p == NULL || p[sep_len] == 0) {
         // no sep, or only one sep at end
         return naf;
      }
      naf = node->m_next;
      if (naf != END) {
         read_buffer(naf);
      }
      else {
         break;
      }
   }

   return END;
}

///////////////////////////////////////////////////////
static NAF narf_unchain(NarfSector length) {
   NAF prev;
   NAF next;
   NAF naf;

   prev = END;
   next = root.m_chain;
   while(next != END) {
      read_buffer(next);
      if (node->m_length >= length) {
         // this will do nicely
         naf = next;
         next = node->m_next;

         // pull it out
         if (prev == END) {
            root.m_chain = next;
         }
         else {
            read_buffer(prev);
            node->m_next = next;
            write_buffer(prev);
         }

         return naf;
      }
      prev = next;
      next = node->m_next;
   }
   return END;
}

//////////////////////////////////////////////////////
//! @brief create a new NAF of given length
//! @see narf_alloc
//! @see narf_realloc
//!
//! assumes narf_begin() has been called
static NAF narf_new(NarfSector length) {
   NAF naf = END;

   if ((root.m_bottom + 2) > (root.m_top - length)) {
      // OUT OF SPACE COLLISSION!!!

      // TODO FIX can we defrag for more room?
      // let's just bail for now.

      return END;
   }

   // maybe we can reclaim a node?
   if (root.m_chain != END) {
      read_buffer(root.m_chain);
      if (node->m_length == 0) {
         naf = root.m_chain;
         root.m_chain = node->m_next;
      }
   }

   if (naf == END) {
      // nope, allocate a new one
      root.m_top -= 2;
      naf = root.m_top;
   }

   // special case
   //
   // initialize the storage to zero
   // and zero out our buffers

   memset(buffer, 0, NARF_SECTOR_SIZE);
   narf_io_write(root.m_origin + naf, buffer);
   narf_io_write(root.m_origin + naf + 1, buffer);

   // we don't know where node is pointing, but it doesn't matter
   // because everything is nulled out

   node->m_start  = length ? root.m_bottom : END;
   node->m_length = length;

   root.m_bottom += length;

   node->m_parent = END;
   node->m_left   = END;
   node->m_right  = END;

   node->m_prev   = END;
   node->m_next   = END;

   write_buffer(naf);

   return naf;
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_alloc(const char *key, NarfByteSize bytes) {
   NAF naf;
   NarfSector length;

   length = BYTES2SECTORS(bytes);

   if (!verify()) return END;

   naf = narf_find(key);

   if (naf != END) {
      return END;
   }

   narf_begin();

   // first check if we can allocate from the chain
   if (length > 0) {
      naf = narf_unchain(length);
   }
   else {
      naf = END;
   }

   if (naf == END) {
      // nothing on the chain was suitable

      naf = narf_new(length);

      if (naf == END) {
         narf_rollback();
         return END;
      }
   }

   // reset fields except start and length
   read_buffer(naf);
   node->m_parent = END;
   node->m_left   = END;
   node->m_right  = END;
   node->m_prev   = END;
   node->m_next   = END;
   node->m_bytes  = bytes;
   memset(node->m_metadata, 0, sizeof(node->m_metadata));
   strncpy(node->m_key, key, KEYSIZE);
   write_buffer(naf);

   ++root.m_count;
   narf_insert(naf, key);

   narf_end();

   return naf;
}

///////////////////////////////////////////////////////
//! @brief copy data and swap data pointers
void narf_copyswap(NAF alpha, NAF beta, NarfByteSize bytes) {
   NarfSector alpha_start;
   NarfSector alpha_length;
   NarfSector beta_start;
   NarfSector beta_length;

   NarfSector i;

   read_buffer(alpha);
   alpha_start = node->m_start;
   alpha_length = node->m_length;

   read_buffer(beta);
   beta_start = node->m_start;
   beta_length = node->m_length;

   for (i = 0; i < alpha_length; ++i) {
      narf_io_read(root.m_origin + alpha_start + i, buffer);
      narf_io_write(root.m_origin + beta_start + i, buffer);
   }

   read_buffer(alpha);
   node->m_start = beta_start;
   node->m_length = beta_length;
   node->m_bytes = bytes;
   write_buffer(alpha);

   read_buffer(beta);
   node->m_start = alpha_start;
   node->m_length = alpha_length;
   write_buffer(beta);
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_realloc(const char *key, NarfByteSize bytes) {
   NAF naf;
   NAF tmp;
   NarfSector new_length;
   NarfSector naf_length;

   if (!verify()) return END;

   naf = narf_find(key);

   if (naf == END) {
      return narf_alloc(key, bytes);
   }

   narf_begin();

   read_buffer(naf);
   new_length = BYTES2SECTORS(bytes);
   naf_length = node->m_length;

   // do we need to change at all?
   if (naf_length >= new_length) {
      // NB: we don't shrink, even if we could!
      node->m_bytes = bytes;
      write_buffer(naf);
      narf_end();
      return naf;
   }
   else {
      // we need to grow

      // first check if we can allocate from the chain
      tmp = narf_unchain(new_length);

      if (tmp == END) {
         // nothing found

         tmp = narf_new(new_length);

         if (tmp == END) {
            // NO ROOM!!!
            narf_rollback();
            return END;
         }
      }

      narf_copyswap(naf, tmp, bytes);
      narf_chain(tmp);
      narf_end();

      return naf;
   }

   // this should never happen
   assert(0);
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_free(const char *key) {
   NAF naf;
   NAF parent, child, left, right;
   NAF prev, next;
   NAF repl;

   if (!verify()) return false;

   naf = narf_find(key);

   if (naf == END) {
      return false;
   }

   narf_begin();

   // unlink from tree
   read_buffer(naf);
   parent = node->m_parent;
   left   = node->m_left;
   right  = node->m_right;
   prev   = node->m_prev;
   next   = node->m_next;

   if (left != END && right != END) {
      // two children

      // excise the prev from the tree

      repl = prev;
      read_buffer(repl);
      parent = node->m_parent;
      child = node->m_left;

      if (child != END) {
         read_buffer(child);
         node->m_parent = parent;
         write_buffer(child);
      }

      read_buffer(parent);
      if (node->m_left == repl) {
         node->m_left = child;
      }
      else if (node->m_right == repl) {
         node->m_right = child;
      }
      else {
         assert(0);
      }
      write_buffer(parent);

      avl_adjust_heights(parent);

      // use repl to replace us

      read_buffer(naf);
      parent = node->m_parent;
      left   = node->m_left;
      right  = node->m_right;

      read_buffer(repl);
      node->m_left = left;
      node->m_right = right;
      node->m_parent = parent;
      write_buffer(repl);

      if (parent == END) {
         root.m_root = repl;
      }
      else {
         read_buffer(parent);
         if (node->m_left == naf) {
            node->m_left = repl;
         }
         else if (node->m_right == naf) {
            node->m_right = repl;
         }
         else {
            assert(0);
         }
         write_buffer(parent);
      }

      if (left != END) {
         read_buffer(left);
         node->m_parent = repl;
         write_buffer(left);
      }

      if (right != END) {
         read_buffer(right);
         node->m_parent = repl;
         write_buffer(right);
      }

      avl_adjust_heights(repl);
      avl_rebalance(repl);
   }
   else {
      // one or no children

      child = (left == END) ? right : left;

      if (parent == END) {
         root.m_root = child;
      }
      else {
         read_buffer(parent);
         if (node->m_left == naf) {
            node->m_left = child;
         }
         else if (node->m_right == naf) {
            node->m_right = child;
         }
         else {
            assert(0);
         }
         write_buffer(parent);

         if (child != END) {
            read_buffer(child);
            node->m_parent = parent;
            write_buffer(child);
         }

         avl_adjust_heights(parent);
         avl_rebalance(parent);
      }
   }

   // unlink from list
   read_buffer(naf);
   prev = node->m_prev;
   next = node->m_next;
   node->m_prev = END;
   node->m_next = END;
   write_buffer(naf);

   if (next != END) {
      read_buffer(next);
      node->m_prev = prev;
      write_buffer(next);
   }
   else {
      root.m_last = prev;
   }

   if (prev != END) {
      read_buffer(prev);
      node->m_next = next;
      write_buffer(prev);
   }
   else {
      root.m_first = next;
   }

   narf_chain(naf);
   --root.m_count;

   narf_end();

   return true;
}

#ifdef NARF_USE_DEFRAG
///////////////////////////////////////////////////////
static void defrag_unchain(NAF naf) {
   NAF prev = END;
   NAF next = root.m_chain;

   while (next != END) {
      if (next == naf) {
         read_buffer(naf);
         next = node->m_next;
         if (prev != END) {
            read_buffer(prev);
            node->m_next = next;
            write_buffer(prev);
         }
         else {
            root.m_chain = next;
         }
         return;
      }
      prev = next;
      read_buffer(next);
      next = node->m_next;
   }
}

///////////////////////////////////////////////////////
//! @brief Combine two adjacent free data spaces
//!
//! this is power loss robust
static void defrag_two_free(NAF free1, NAF free2) {
   NarfSector length;

   narf_begin();

   defrag_unchain(free1);
   defrag_unchain(free2);

   read_buffer(free2);
   length = node->m_length;
   node->m_start = END;
   node->m_length = 0;
   write_buffer(free2);

   narf_chain(free2);

   read_buffer(free1);
   node->m_length += length;
   write_buffer(free1);

   narf_chain(free1);

   narf_end();
}

///////////////////////////////////////////////////////
//! @brief handle adjacent free and tree nodes
//!
//! this is power loss robust
static void defrag_free_tree(NAF free1, NAF tree2) {
   NarfSector f_start;
   NarfSector f_length;
   NarfSector t_start;
   NarfSector t_length;
   NarfSector i;

   read_buffer(free1);
   f_start = node->m_start;
   f_length = node->m_length;

   read_buffer(tree2);
   t_start = node->m_start;
   t_length = node->m_length;

   if (f_length >= t_length) {
      // easy case, non overlapping
      for (i = 0; i < t_length; i++) {
         narf_io_read(root.m_origin + t_start + i, buffer);
         narf_io_write(root.m_origin + f_start + i, buffer);
      }

      narf_begin();
      defrag_unchain(free1);

      read_buffer(tree2);
      node->m_start = f_start;
      write_buffer(tree2);

      read_buffer(free1);
      node->m_start = f_start + t_length;
      write_buffer(free1);

      narf_chain(free1);
      narf_end();
   }
   else {
      // overwriting valid data is bad for power loss
      // so we ALLOCATE new data and move it there!
      // hopefully this will let us move other stuff down.
      for (i = 0; i < t_length; i++) {
         narf_io_read(root.m_origin + t_start + i, buffer);
         narf_io_write(root.m_origin + root.m_bottom + i, buffer);
      }

      narf_begin();
      defrag_unchain(free1);

      read_buffer(tree2);
      node->m_start = root.m_bottom;
      root.m_bottom += t_length;
      write_buffer(tree2);

      read_buffer(free1);
      node->m_length += t_length;
      write_buffer(free1);

      narf_chain(free1);
      narf_end();
   }
}

///////////////////////////////////////////////////////
//! @brief Carve out unneeded room from data
static void defrag_carve(void) {
   NAF tmp;
   NAF next;
   NarfSector start;
   NarfSector length;
   NarfSector new_start;
   NarfSector new_length;

   for (tmp = root.m_first; tmp != END; tmp = next) {
      read_buffer(tmp);
      next = node->m_next;
      start = node->m_start;
      length = BYTES2SECTORS(node->m_bytes);

      if (node->m_length > length) {
         narf_begin();

         read_buffer(tmp);

         new_start = start + length;
         new_length = node->m_length - length;

         node->m_length = length;
         write_buffer(tmp);

         tmp = narf_new(0);

         read_buffer(tmp);
         node->m_start = new_start;
         node->m_length = new_length;
         write_buffer(tmp);

         narf_chain(tmp);

         narf_end();
      }
   }
}

///////////////////////////////////////////////////////
//! @brief squish data down to lower root.m_bottom
static void defrag_squish(void) {
   NAF tmp;
   NAF lowest;
   NarfSector start;
   NarfSector length;

   while (1) {
      another:
      // find lowest free node
      lowest = END;
      for (tmp = root.m_chain; tmp != END; tmp = node->m_next) {
         read_buffer(tmp);
         if (node->m_start != END) {
            if (lowest == END || node->m_start < start) {
               start = node->m_start;
               length = node->m_length;
               lowest = tmp;
            }
         }
      }

      if (lowest == END) {
         break;
      }

      // find the node with successor data, which could be on
      // the chain OR in the tree

      // check the chain first
      for (tmp = root.m_chain; tmp != END; tmp = node->m_next) {
         read_buffer(tmp);
         if (node->m_start == start + length) {
            // we found it, let's combine them.
            defrag_two_free(lowest, tmp);
            goto another;
         }
      }

      // check the tree next
      for (tmp = root.m_first; tmp != END; tmp = node->m_next) {
         read_buffer(tmp);
         if (node->m_start == start + length) {
            // we found it, let's combine them.
            defrag_free_tree(lowest, tmp);
            goto another;
         }
      }

      // if we made it this far, there was no successor
      if (start + length == root.m_bottom) {
         narf_begin();
         read_buffer(lowest);
         node->m_start = END;
         node->m_length = 0;
         write_buffer(lowest);
         root.m_bottom = start;
         narf_end();
         goto another;
      }

      assert(0);
   }
}

///////////////////////////////////////////////////////
//! @brief return true if a NAF is in the chain
static bool defrag_ischained(NAF naf) {
   NAF tmp;
   for (tmp = root.m_chain; tmp != END; tmp = node->m_next) {
      if (naf == tmp) {
         return true;
      }
      read_buffer(tmp);
   }
   return false;
}

///////////////////////////////////////////////////////
//! @brief shuffle NAFs up to increase root.m_top
static void defrag_tidy(void) {
   NAF tmp;
   NAF parent, left, right;
   NAF prev, next;

   while (root.m_chain != END) {
      if (defrag_ischained(root.m_top)) {
         narf_begin();
         defrag_unchain(root.m_top);
         root.m_top += 2;
         narf_end();
      }
      else {
         tmp = root.m_chain;
         read_buffer(tmp);
         if (node->m_length == 0) {
            narf_begin();

            root.m_chain = node->m_next;

            read_buffer(root.m_top);

            prev = node->m_prev;
            next = node->m_next;

            parent = node->m_parent;
            left = node->m_left;
            right = node->m_right;

            write_buffer(tmp);

            if (prev != END) {
               read_buffer(prev);
               node->m_next = tmp;
               write_buffer(prev);
            }
            else {
               root.m_first = tmp;
            }

            if (next != END) {
               read_buffer(next);
               node->m_prev = tmp;
               write_buffer(next);
            }
            else {
               root.m_last = tmp;
            }

            if (left != END) {
               read_buffer(left);
               node->m_parent = tmp;
               write_buffer(left);
            }

            if (right != END) {
               read_buffer(right);
               node->m_parent = tmp;
               write_buffer(right);
            }

            if (parent != END) {
               read_buffer(parent);
               if (node->m_left == root.m_top) {
                  node->m_left = tmp;
               }
               else if (node->m_right == root.m_top) {
                  node->m_right = tmp;
               }
               else {
                  assert(0);
               }
               write_buffer(parent);
            }
            else {
               root.m_root = tmp;
            }

            root.m_top += 2;

            narf_end();
         }
      }
   }
}

///////////////////////////////////////////////////////
//! @brief update all generation numbers to be recent
static void defrag_regen(void) {
   NAF i;

   // update generation numbers for all nodes to be
   // recent.
   for (i = root.m_top; i < root.m_total_sectors; i += 2) {
      narf_begin();
      read_buffer(i);
      write_buffer(i);
      narf_end();
   }
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_defrag(void) {
   if (!verify()) return false;

   // to maintain power loss robustness, we need to be
   // very careful.  in particular, we can't overwrite
   // data.  so we need to do this in small steps.

   // first, carve free space from over long files
   defrag_carve();

   // second, squish down
   defrag_squish();

   // third, tidy up nodes.
   defrag_tidy();

   // fourth, renew generations.
   defrag_regen();

   return true;
}
#endif

///////////////////////////////////////////////////////
//! @see narf.h
const char *narf_key(NAF naf) {
   if (!verify() || naf == END) return NULL;
   read_buffer(naf);
   return node->m_key;
}

///////////////////////////////////////////////////////
//! @see narf.h
NarfSector narf_sector(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->m_start;
}

///////////////////////////////////////////////////////
//! @see narf.h
NarfByteSize narf_size(NAF naf) {
   if (!verify() || naf == END) return 0;
   read_buffer(naf);
   return node->m_bytes;
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_first(void) {
   if (!verify()) return END;
   return root.m_first;
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_next(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->m_next;
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_last(void) {
   if (!verify()) return END;
   return root.m_last;
}

///////////////////////////////////////////////////////
//! @see narf.h
NAF narf_previous(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->m_prev;
}

///////////////////////////////////////////////////////
//! @see narf.h
void *narf_metadata(NAF naf) {
   if (!verify() || naf == END) return NULL;
   read_buffer(naf);
   return node->m_metadata;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_set_metadata(NAF naf, void *data) {
   if (!verify() || naf == END) return false;

   narf_begin();

   read_buffer(naf);
   memcpy(node->m_metadata, data, sizeof(node->m_metadata));
   write_buffer(naf);

   narf_end();

   return true;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_append(const char *key, const void *data, NarfByteSize size) {
   NarfByteSize og_bytes;
   NarfByteSize begin;
   NarfByteSize remain;
   NarfSector start;
   NarfSector current;
   NAF naf = narf_find(key);

   if (naf == END) {
      return false;
   }

   read_buffer(naf);
   og_bytes = node->m_bytes;

   naf = narf_realloc(key, og_bytes + size);

   if (naf == END) {
      return false;
   }

   read_buffer(naf);
   start = node->m_start;

   begin = og_bytes % NARF_SECTOR_SIZE;
   current = og_bytes / NARF_SECTOR_SIZE; // TODO FIX ???
   remain = NARF_SECTOR_SIZE - begin;

   while (size) {
      narf_io_read(root.m_origin + start + current, buffer);
      if (remain > size) {
         remain = size;
      }
      memcpy(buffer + begin, data, remain);
      narf_io_write(root.m_origin + start + current, buffer);

      // advance all our pointers
      data = (uint8_t *) data + remain;
      size -= remain;
      og_bytes += remain;
      ++current;
      begin = 0;
      remain = NARF_SECTOR_SIZE;
   }

   return true;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
