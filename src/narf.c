#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf_conf.h"
#include "narf.h"
#include "narf_io.h"

#ifdef NARF_MALLOC
   #include <stdlib.h>
#endif

#ifdef NARF_DEBUG
   #include <assert.h>
   #include <stdlib.h>
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
   NarfSector   m_count;         // count of total number of allocated NAF
   NarfSector   m_vacant;        // number of first unallocated sector
   NarfSector   m_start;         // where the root sector lives

   // TODO FIX this is wasted space, fix it.
   uint8_t      m_reserved[ NARF_SECTOR_SIZE -
                            1 * sizeof(NarfByteSize) -
                            4 * sizeof(NarfSector) -
                            4 * sizeof(NAF) -
                            5 * sizeof(uint32_t) ];

   uint32_t     m_generation;  // the generation number
   uint32_t     m_random;      // lfsr sequence number
   uint32_t     m_checksum;    // crc32
} Root;
static_assert(sizeof(Root) == NARF_SECTOR_SIZE, "Root wrong size");

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

   uint8_t m_metadata[128]; // not used by NARF

   char         m_key     [ NARF_SECTOR_SIZE -
                            5 * sizeof(NAF) -
                            2 * sizeof(NarfSector) -
                            1 * sizeof(NarfByteSize) -
                            128 -
                            3 * sizeof(uint32_t) ];

   uint32_t     m_generation;  // the generation number
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

static uint32_t entropy = 0xdeadbeef;

static uint8_t buffer_lo[NARF_SECTOR_SIZE] = { 0 };
static uint8_t buffer_hi[NARF_SECTOR_SIZE] = { 0 };
static uint8_t *buffer = buffer_lo;

static bool write_to_hi = false;
static bool write_root_to_hi;

static Root root = { 0 };
static Root * const root_hi = (Root *) buffer_hi;
static Root * const root_lo = (Root *) buffer_lo;

static Node * const node_hi = (Node *) buffer_hi;
static Node * const node_lo = (Node *) buffer_lo;
static Node *node = node_lo;

///////////////////////////////////////////////////////
//! @brief crc32 checksum
uint32_t crc32(const void *data, int length) {
    uint32_t crc = 0xFFFFFFFF; // Initial value
    int i;
    for (i = 0; i < length; i++) {
        crc ^= *((uint8_t *)data++); // XOR with input byte
        for (int j = 0; j < 8; j++) { // Process 8 bits
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320; // Polynomial for CRC-32
            else
                crc >>= 1;
        }
    }
    return ~crc; // Final XOR
}

///////////////////////////////////////////////////////
//! @brief Read a NAF into our buffer
//!
//! @param naf The naf to read
//! @return true on success
static bool read_buffer(NAF naf) {
   int32_t tmp;
   uint32_t gen_lo;
   uint32_t gen_hi;
   uint32_t ck_lo;
   uint32_t ck_hi;

   // we don't want to get confused by failed reads
   // and stale data.  we also want to increase entropy.
   // so we load the buffers with random data before
   // we do a read.  this will frustrate crc32 check
   // later.
   node_lo->m_random = lrand48();
   node_hi->m_random = lrand48();

   if (!narf_io_read(root.m_start + naf, buffer_lo)) return false;
   if (!narf_io_read(root.m_start + naf + 1, buffer_hi)) return false;

   ck_lo = crc32(buffer_lo, NARF_SECTOR_SIZE - sizeof(uint32_t));
   ck_hi = crc32(buffer_hi, NARF_SECTOR_SIZE - sizeof(uint32_t));

   if (ck_lo == node_lo->m_checksum) {
      if (ck_hi == node_hi->m_checksum) {

         // both checksums good, we can trust the data
         gen_lo = node_lo->m_generation;
         gen_hi = node_hi->m_generation;

         if (gen_lo <= root.m_generation) {
            if (gen_hi <= root.m_generation) {
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
            if (gen_hi <= root.m_generation) {
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
         buffer = buffer_lo;
         node = node_lo;
         write_to_hi = (node->m_generation != root.m_generation);
      }
   }
   else {
      if (ck_hi == node_hi->m_checksum) {
         // only hi is good
hi_is_good: 
         buffer = buffer_hi;
         node = node_hi;
         write_to_hi = (node->m_generation == root.m_generation);
      }
      else {
         // this should never happen
         assert(0);
         return false;
      }
   }

   // oi.  if we're here, both buffers are the same
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
   node->m_checksum = crc32(node, NARF_SECTOR_SIZE - sizeof(uint32_t));

   return narf_io_write(root.m_start + naf + (write_to_hi ? 1 : 0), buffer);
}

#define strncmp utf8_strncmp

///////////////////////////////////////////////////////
//! @brief Decode UTF-8
//!
//! Decode a UTF-8 sequence into a Unicode code point (handles incomplete
//! sequences)
//!
//! also adds entropy by inserting calls to lrand48()
//!
static int32_t utf8_decode_safe(const char **s, const char *end) {
   const unsigned char *p = (const unsigned char *) *s;
   int codepoint = 0;
   int num_bytes = 0;

   if (*s >= end || !**s) return -2;  // Stop at buffer end or null terminator

   if (p[0] < 0x80) {  // 1-byte (ASCII)
      codepoint = p[0];
      num_bytes = 1;
   } else if (p[0] >= 0xC2 && p[0] <= 0xDF && *s + 1 < end) {  // 2-byte
      codepoint = (p[0] & 0x1F) << 6 |
         (p[1] & 0x3F);
      num_bytes = 2;
   } else if (p[0] >= 0xE0 && p[0] <= 0xEF && *s + 2 < end) {  // 3-byte
      codepoint = (p[0] & 0x0F) << 12 |
         (p[1] & 0x3F) <<  6 |
         (p[2] & 0x3F);
      num_bytes = 3;
   } else if (p[0] >= 0xF0 && p[0] <= 0xF4 && *s + 3 < end) {  // 4-byte
      codepoint = (p[0] & 0x07) << 18 |
         (p[1] & 0x3F) << 12 |
         (p[2] & 0x3F) <<  6 |
         (p[3] & 0x3F);
      num_bytes = 4;
   } else {
      return -1; // Invalid UTF-8 sequence
   }

   if (codepoint & 1) {
      entropy ^= lrand48();
   }

   *s += num_bytes;
   return codepoint;
}

///////////////////////////////////////////////////////
//! @brief UTF-8 aware strncmp
//!
//! compares up to n bytes, ensuring character integrity
//!
static int16_t utf8_strncmp(const char *s1, const char *s2, size_t n) {
   const char *end1 = s1 + n;
   const char *end2 = s2 + n;

   while (s1 < end1 && s2 < end2 && *s1 && *s2) {
      const char *prev_s1 = s1;
      const char *prev_s2 = s2;
      int cp1 = utf8_decode_safe(&s1, end1);
      int cp2 = utf8_decode_safe(&s2, end2);

      if (cp1 == -1 || cp2 == -1) {
         // Invalid UTF-8 fallback
         return (unsigned char)*prev_s1 - (unsigned char)*prev_s2;
      }
      if (cp1 == -2 || cp2 == -2) {
         // Reached byte limit safely
         assert(0);
         break;
      }
      if (cp1 != cp2) {
         return (cp1 > cp2) ? 1 : -1; // cp1 - cp2;
      }
   }

   if (*s1 > *s2) return 1;
   if (*s1 < *s2) return -1;
   return 0;
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
   MBR *mbr = (MBR *) buffer_lo;

   if (!narf_io_open()) return false;

   memset(buffer_lo, 0, sizeof(buffer_lo));
   memcpy(buffer_lo, boot_code_stub, sizeof(boot_code_stub));
   if (message == NULL) {
      message = boot_code_msg;
   }
   strcpy((char *)(buffer_lo + sizeof(boot_code_stub)), message);
   mbr->signature = MBR_SIGNATURE;
   narf_io_write(0, buffer_lo);

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
   mbr = (MBR *) buffer_lo;
   narf_io_read(0, buffer_lo);

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

   narf_io_write(0, buffer_lo);

   return true;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_format(int partition) {
   MBR *mbr;

   if (!narf_io_open()) return false;

   mbr = (MBR *) buffer_lo;
   narf_io_read(0, buffer_lo);

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

   mbr = (MBR *) buffer_lo;
   narf_io_read(0, buffer_lo);

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

   mbr = (MBR *) buffer_lo;
   narf_io_read(0, buffer_lo);

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
   NAF l;
   NAF r;
   int i;
   char *p;
   char *arm;

   if (!verify()) return;

   if (naf != END) {
      read_buffer(naf);
      l = node->m_left;
      r = node->m_right;
      p = strdup(node->m_key);
      narf_pt(l, indent + 1, pattern);
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
      printf("%s %s [%d]", arm, p, naf);
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
      free(p);
   }

   narf_pt(r, indent + 1, (pattern ^ (3 << (indent))) & ~1);
}

///////////////////////////////////////////////////////
//! @see narf_debug
static void print_node(NAF naf) {
   read_buffer(naf);
   printf("naf = %d => '%.*s'\n",
         naf, (int) KEYSIZE, node->m_key);
   printf("tree u/l/r  = %d / %d / %d\n",
         node->m_parent, node->m_left, node->m_right);
   printf("list p/n    = %d / %d\n",
         node->m_prev, node->m_next);
   printf("start:len   = %d:%d (%d)\n",
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
         printf("%d (%d:%d) -> %d\n", naf, node->m_start, node->m_length, node->m_next);
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

   printf("root.m_total_sectors = %d\n", root.m_total_sectors);
   if (root.m_total_sectors < 2) {
      printf("bad total sectors\n");
      return;
   }

   printf("root.m_vacant        = %d\n", root.m_vacant);
   printf("root.m_chain         = %d\n", root.m_chain);
   printf("root.m_root          = %d\n", root.m_root);
   printf("root.m_first         = %d\n", root.m_first);
   printf("root.m_last          = %d\n", root.m_last);
   printf("root.m_count         = %d\n", root.m_count);
   printf("root.m_start         = %d\n", root.m_start);
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

      assert(a < root.m_vacant);

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
      b = node->m_next;
      if (b != END) {
         read_buffer(b);
         b = node->m_next;
      }

      if (a != END) {
         read_buffer(a);
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

   if (root.m_first == END && root.m_last == END) {
      return;
   }

   assert(root.m_first != END && root.m_last != END);

   // walk forward
   prev = END;
   naf = root.m_first;

   while (naf != END) {
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

   // walk backward
   next = END;
   naf = root.m_last;

   while (naf != END) {
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
}

///////////////////////////////////////////////////////
static void verify_integrity(void) {
   printf("verify integrity order_loop\n");
   order_loop();
   printf("verify integrity reverse_loop\n");
   reverse_loop();
   printf("verify integrity chain_loop\n");
   chain_loop();
   printf("verify integrity walk_chain\n");
   walk_chain();
   printf("verify integrity walk_tree\n");
   walk_tree(END, root.m_root);
   printf("verify integrity prev_next_cohesion\n");
   walk_order();
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
   NAF tmp;
   NarfSector length;
   NarfSector tmp_length;

again:

   // reset fields
   read_buffer(naf);
   node->m_prev = END;
   node->m_next = END;
   node->m_left = END;
   node->m_right = END;
   node->m_parent = END;
   node->m_bytes = 0;
   // do NOT reset "start" and "length"
   length = node->m_length;
   write_buffer(naf);

#ifdef NARF_DEBUG_INTEGRITY
   printf("chaining...\n");
   print_node(naf);
#endif

   // can they be combined with another?
   prev = END;
   next = root.m_chain;
   while(next != END) {

      read_buffer(next);

      // anticipation...
      tmp = next;
      tmp_length = node->m_length;
      next = node->m_next;

      // are the two adjacent?
      if ((naf == tmp + tmp_length + 2) ||
            (naf + length + 2 == tmp)) {
         // remove item from chain
         if (prev == END) {
            root.m_chain = next;
         }
         else {
            read_buffer(prev);
            node->m_next = next;
            write_buffer(prev);
         }
#ifdef NARF_DEBUG_INTEGRITY
         printf("combining %d %d\n", naf, tmp);
         print_node(naf);
         print_node(tmp);
#endif
         // combine the two
         if (tmp < naf) {
            read_buffer(tmp);
            node->m_length += length + 2;
            write_buffer(tmp);
            naf = tmp;
         }
         else {
            read_buffer(naf);
            node->m_length += tmp_length + 2;
            write_buffer(naf);
         }
         // reinsert
         goto again;
      }

      prev = tmp;
      // next has already been set!
   }

   // dumbest case, can we rewind root.m_vacant?
   if (root.m_vacant == naf + length + 2) {
#ifdef NARF_DEBUG_INTEGRITY
      printf("rewind to %d\n", naf);
#endif
      root.m_vacant = naf;
      return;
   }

   // reset the buffer
   read_buffer(naf);

   // record them in the free chain
   if (root.m_chain == END) {
#ifdef NARF_DEBUG_INTEGRITY
      printf("head of chain\n");
#endif
      // done above // read_buffer(naf);
      node->m_next = root.m_chain;
      write_buffer(naf);

      root.m_chain = naf;
   }
   else {
      // smallest records first
      // done above // read_buffer(naf);
      length = node->m_length;

      prev = END;
      next = root.m_chain;
      read_buffer(next);

      while (length > node->m_length && next != END) {
         prev = next;
         next = node->m_next;
         if (next != END) {
            read_buffer(next);
         }
      }
      if (prev == END) {
         root.m_chain = naf;
      }
      else {
         read_buffer(prev);
         node->m_next = naf;
         write_buffer(prev);
      }
      read_buffer(naf);
      node->m_next = next;
      write_buffer(naf);
   }

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif
}

///////////////////////////////////////////////////////
//! @brief Insert NAF into the tree and list.
//!
//! @return true for success
static bool narf_insert(NAF naf, const char *key) {
   NAF tmp;
   NAF p;
   int cmp;
   int height;

   height = 0;

   if (!verify()) return false;

   if (root.m_root == END) {
      root.m_root = naf;
      root.m_first = naf;
      root.m_last = naf;
   }
   else {
      p = root.m_root;
      while (1) {
         read_buffer(p);
         cmp = strncmp(key, node->m_key, KEYSIZE);
         if (cmp < 0) {
            if (node->m_left != END) {
               p = node->m_left;
               ++height;
            }
            else {
               // we are the new left of p
               node->m_left = naf;
               tmp = node->m_prev;
               node->m_prev = naf;
               write_buffer(p);

               read_buffer(naf);
               node->m_parent = p;
               node->m_prev = tmp;
               node->m_next = p;
               write_buffer(naf);

               if (tmp != END) {
                  read_buffer(tmp);
                  node->m_next = naf;
                  write_buffer(tmp);
               }
               else {
                  root.m_first = naf;
               }

               break;
            }
         }
         else if (cmp > 0) {
            if (node->m_right != END) {
               p = node->m_right;
               ++height;
            }
            else {
               // we are the new right of p
               node->m_right = naf;
               tmp = node->m_next;
               node->m_next = naf;
               write_buffer(p);

               read_buffer(naf);
               node->m_parent = p;
               node->m_next = tmp;
               node->m_prev = p;
               write_buffer(naf);

               if (tmp != END) {
                  read_buffer(tmp);
                  node->m_prev = naf;
                  write_buffer(tmp);
               }
               else {
                  root.m_last = naf;
               }

               break;
            }
         }
         else {
            // this should never happen !!!
            assert(0);
         }
      }
   }

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

   return true;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_mkfs(NarfSector start, NarfSector size) {
   if (!narf_io_open()) return false;

   memset(buffer_lo, 0, NARF_SECTOR_SIZE);

   root.m_signature     = SIGNATURE;
   root.m_version       = VERSION;
   root.m_sector_size   = NARF_SECTOR_SIZE;
   root.m_total_sectors = size;
   root.m_vacant        = 2;
   root.m_root          = END;
   root.m_first         = END;
   root.m_last          = END;
   root.m_chain         = END;
   root.m_count         = 0;
   root.m_start         = start;

   root.m_generation    = 0;
   root.m_random        = lrand48();
   root.m_checksum      = crc32(&root, NARF_SECTOR_SIZE - sizeof(uint32_t));

   memcpy(buffer_lo, &root, sizeof(root));
   narf_io_write(start, buffer_lo);

   root.m_random        = lrand48();
   root.m_checksum      = crc32(&root, NARF_SECTOR_SIZE - sizeof(uint32_t));

   memcpy(buffer_lo, &root, sizeof(root));
   narf_io_write(start + 1, buffer_lo);

#ifdef NARF_DEBUG
   printf("keysize %ld\n", KEYSIZE);
#endif

   return true;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_init(NarfSector start) {
   bool ret;
   int32_t tmp;
   uint32_t gen_lo;
   uint32_t gen_hi;
   uint32_t ck_lo;
   uint32_t ck_hi;

#ifdef NARF_DEBUG
   printf("keysize %ld\n", KEYSIZE);
#endif

   if (!narf_io_open()) return false;

   ret = narf_io_read(start, buffer_lo);
   if (!ret) return false;

   ret = narf_io_read(start + 1, buffer_hi);
   if (!ret) return false;

   ck_lo = crc32(root_lo, NARF_SECTOR_SIZE - sizeof(uint32_t));
   ck_hi = crc32(root_hi, NARF_SECTOR_SIZE - sizeof(uint32_t));

   if (ck_lo == root_lo->m_checksum) {
      if (ck_hi == root_hi->m_checksum) {
         // both good, compare generations
         gen_lo = root_lo->m_generation;
         gen_hi = root_hi->m_generation;

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
         memcpy(&root, root_lo, sizeof(Root));
         write_root_to_hi = true;
      }
   }
   else {
      if (ck_hi == root_hi->m_checksum) {
         // hi good
hi_is_good:
         memcpy(&root, root_hi, sizeof(Root));
         write_root_to_hi = false;
      }
      else {
         // none good

         // we can't do anything in this case
         assert(0);
         return false;
      }
   }

   return verify();
}

static int semaphore = 0;

///////////////////////////////////////////////////////
//! @brief begin a transaction
static void narf_begin(void) {
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
      root.m_checksum = crc32(&root, NARF_SECTOR_SIZE - sizeof(uint32_t));
      write_root_to_hi = !write_root_to_hi;
      narf_io_write(root.m_start + (write_root_to_hi ? 0 : 1), &root);
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
            naf = node->m_prev;
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

#ifdef NARF_DEBUG_INTEGRITY
         printf("NEED %d FOUND %d %d:%d\n",
               length, next, node->m_start, node->m_length);
         print_node(next);
#endif

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

#ifdef NARF_DEBUG_INTEGRITY
         verify_integrity();
#endif

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
   NAF naf;

   if (root.m_vacant + length + 2 > root.m_total_sectors) {
      // NO ROOM!!!
      return END;
   }

   naf = root.m_vacant;

   // special case
   // initialize the storage to zero
   // and zero out our buffers
   memset(buffer_lo, 0, NARF_SECTOR_SIZE);
   narf_io_write(naf, buffer_lo);
   memset(buffer_hi, 0, NARF_SECTOR_SIZE);
   narf_io_write(naf + 1, buffer_hi);

   root.m_vacant += 2;
   node->m_start  = length ? root.m_vacant : END;
   node->m_length = length;
   root.m_vacant += length;
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

#ifdef NARF_DEBUG_INTEGRITY
   printf("alloc %08x %08x %d %d\n",
         naf, node->m_start, node->m_length, node->m_bytes);
#endif

   ++root.m_count;
   narf_insert(naf, key);

   narf_end();

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

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
      narf_io_read(root.m_start + alpha_start + i, buffer_lo);
      narf_io_write(root.m_start + beta_start + i, buffer_lo);
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

#ifdef NARF_DEBUG_INTEGRITY
      verify_integrity();
#endif
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
      printf("two children\n");

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
   }
   else {
      // one or no children
      printf("not two children\n");

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

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_rebalance(void) {
#ifdef NARF_MALLOC
   char *key;
#else
   static char key[KEYSIZE]; // TODO/FIX EXPENSIVE !!!
#endif

   NAF head = root.m_first;

   NAF naf = root.m_first;
   NarfSector count = 0;
   NarfSector target = 0;
   NarfSector spot = 0;

   NarfSector numerator;
   NarfSector denominator = 2;

   NAF prev;
   NAF next;

   if (!verify()) {
      return false;
   }

#ifdef NARF_MALLOC
   key = malloc(KEYSIZE);
#endif

   // get an accurate count of nodes
   while (naf != END) {
      ++count;
      read_buffer(naf);
      naf = node->m_next;
   }

   narf_begin();

   root.m_root = END;
   root.m_first = END;
   root.m_last = END;

   while (denominator < count) {
      // odd multiples of denominator
      naf = head;
      numerator = 1;
      target = count * numerator / denominator;
      spot = 0;

      while (numerator < denominator && naf != END) {
         read_buffer(naf);
         while (naf != END) {
            next = node->m_next;
            if (spot == target) {
               prev = node->m_prev;

               if (head == naf) {
                  head = next;
               }
               if (prev != END) {
                  read_buffer(prev);
                  node->m_next = next;
                  write_buffer(prev);
               }
               if (next != END) {
                  read_buffer(next);
                  node->m_prev = prev;
                  write_buffer(next);
               }

               read_buffer(naf);
               node->m_prev = END;
               node->m_next = END;
               node->m_left = END;
               node->m_right = END;
               node->m_parent = END;
               strncpy(key, node->m_key, KEYSIZE);
               write_buffer(naf);

               narf_insert(naf, key);

               numerator += 2;
               target = count * numerator / denominator;
            }
            ++spot;
            naf = next;
            if (naf != END) {
               read_buffer(naf);
            }
         }
      }

      count = count - denominator / 2;
      denominator *= 2;
   }

   // now finish the job
   naf = head;
   while (naf != END) {
      read_buffer(naf);
      next = node->m_next;
      node->m_prev = END;
      node->m_next = END;
      node->m_left = END;
      node->m_right = END;
      node->m_parent = END;
      strncpy(key, node->m_key, KEYSIZE);
      write_buffer(naf);

      narf_insert(naf, key);

      naf = next;
   }

   narf_end();

#ifdef NARF_MALLOC
   free(key);
#endif

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif

   return true;
}

///////////////////////////////////////////////////////
//! @see narf.h
bool narf_defrag(void) {
#if 0
   NAF tmp;
   NAF other;
   NAF parent;
   NAF left;
   NAF right;
   NAF prev;
   NAF next;
   NarfSector tmp_length;
   NarfSector other_length;
   NarfSector i;

   narf_begin();

   while (root.m_chain != END) {
      tmp = root.m_chain;
      read_buffer(tmp);
      root.m_chain = node->m_next;
      tmp_length = node->m_length;

      other = tmp + tmp_length + 2;
      read_buffer(other);
      other_length = node->m_length;
      parent = node->m_parent;
      left = node->m_left;
      right = node->m_right;
      prev = node->m_prev;
      next = node->m_next;
      node->m_start = tmp + 2;
      write_buffer(tmp);

      for (i = 0; i < other_length; ++i) {
         narf_io_read(other + i + 2, buffer_lo);
         narf_io_write(tmp + i + 2, buffer_lo); // bypass integrity
      }

      if (parent != END) {
         read_buffer(parent);
         if (node->m_left == other) {
            node->m_left = tmp;
         }
         else if (node->m_right == other) {
            node->m_right = tmp;
         }
         else {
            // this should never happen
            assert(0);
         }
         write_buffer(parent);
      }
      else {
         root.m_root = tmp;
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

      other = tmp + other_length + 2;

      // we're building a new node from junk data,
      // and we're just going to chain it,
      // so there's no point in doing a read here.

      // in fact, we need to destructively blank
      // some stuff.

      memset(buffer_lo, 0, NARF_SECTOR_SIZE);
      memset(buffer_hi, 0, NARF_SECTOR_SIZE);
      narf_io_write(other, buffer_lo);
      narf_io_write(other + 1, buffer_hi);

      node->m_start = other + 2;
      node->m_length = tmp_length;
      write_buffer(other);

      narf_chain(other);
   }

   narf_end();

#ifdef NARF_DEBUG_INTEGRITY
   verify_integrity();
#endif
#endif
   return true;
}

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
      narf_io_read(start + current, buffer_lo);
      if (remain > size) {
         remain = size;
      }
      memcpy(buffer + begin, data, remain);
      narf_io_write(start + current, buffer_lo); // bypass integrity

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
