#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf.h"
#include "narf_io.h"

#ifdef NARF_DEBUG
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#endif

#ifdef __GNUC__
#define PACKED __attribute__((packed))
#else
#define PACKED
#endif

#define SIGNATURE 0x4652414E // FRAN => NARF
#define VERSION 0x00000000

#define END INVALID_NAF // i hate typing

#define SECTOR_SIZE 512

// 32 is the ideal height of a binary
// tree with 2^32 nodes.
#define FORCE_REBALANCE 32

//! @brief The Root structure for our Not A Real Filesystem
//!
//! it is kept in memory, and flushed out with narf_sync().
//! it is intentionally small.
typedef struct PACKED {
   union {
      uint32_t signature;  // SIGNATURE
      uint8_t sigbytes[4];
   };
   uint32_t version;       // VERSION
   uint32_t sector_size;   // sector size in bytes
   uint32_t total_sectors; // total size of storage in sectors
   NAF root;               // sector of root node
   NAF first;              // sector of first node in key order
   NAF last;               // sector of last node in key order
   NAF chain;              // previously allocated but free now
   uint32_t vacant;        // number of first unallocated sector
} Root;
static_assert(sizeof(Root) == 9 * sizeof(uint32_t), "Root wrong size");

// TODO FIX is "Header" really still an appropriate name for this?
typedef struct PACKED {
   NAF parent;      // parent NAF
   NAF left;        // left sibling NAF
   NAF right;       // right sibling NAF
   NAF prev;        // previous ordered NAF
   NAF next;        // next ordered NAF

   uint32_t start;       // data start sector
   uint32_t length;      // data length in sectors
   uint32_t bytes;       // data size in bytes

   char key[512 - 8 * sizeof(uint32_t)]; // key
} Header;
static_assert(sizeof(Header) == 512, "Header wrong size");

uint8_t buffer[SECTOR_SIZE] = { 0 };
Root root = { 0 };
Header *node = (Header *) buffer;

//! @brief Read a NAF into our buffer
//!
//! @param naf The naf to read
//! @return true on success
static bool read_buffer(NAF naf) {
   return narf_io_read(naf, buffer);
}

//! @brief Write a NAF from buffer to disk
//!
//! @param naf The NAF to write
//! @return true on success
static bool write_buffer(NAF naf) {
   return narf_io_write(naf, buffer);
}

//! @brief Verify we're working with a valid filesystem
//!
//! determines if init() or mkfs() was called and filesystem is valid
//!
//! @return true on success
static bool verify(void) {
   if (root.signature != SIGNATURE) return false;
   if (root.version != VERSION) return false;
   if (root.sector_size != SECTOR_SIZE) return false;
   return true;
}

//! @brief add a naf to the free chain
//!
//! @param naf The NAF to add
static void narf_chain(NAF naf) {
   uint32_t prev;
   uint32_t next;
   uint32_t length;

   // reset fields
   read_buffer(naf);
   node->prev = END;
   node->next = END;
   node->left = END;
   node->right = END;
   node->parent = END;
   node->bytes = 0;
   // do NOT reset "start" and "length"
   write_buffer(naf);

   // record them in the free chain
   if (root.chain == END) {
      // done above // read_buffer(naf);
      node->next = root.chain;
      write_buffer(naf);

      root.chain = naf;
      narf_sync();
   }
   else {
      // smallest records first
      // done above // read_buffer(naf);
      length = node->length;

      prev = END;
      next = root.chain;
      read_buffer(next);

      while (length > node->length && next != END) {
         prev = next;
         next = node->next;
         if (next != END) {
            read_buffer(next);
         }
      }
      if (prev == END) {
         root.chain = naf;
         narf_sync();
      }
      else {
         read_buffer(prev);
         node->next = naf;
         write_buffer(prev);
      }
      read_buffer(naf);
      node->next = next;
      write_buffer(naf);
   }
}

//! @brief Insert sector into the tree and list.
//!
//! Forces rebalance if tree is too tall.
//!
//! @return true for success
static bool narf_insert(NAF naf, const char *key) {
   NAF tmp;
   NAF p;
   int cmp;
   int height;

   height = 0;

   if (!verify()) return false;

   if (root.root == END) {
      root.root = naf;
      root.first = naf;
      narf_sync();
   }
   else {
      p = root.root;
      while (1) {
         read_buffer(p);
         cmp = strncmp(key, node->key, sizeof(node->key));
         if (cmp < 0) {
            if (node->left != END) {
               p = node->left;
               ++height;
            }
            else {
               // we are the new left of p
               node->left = naf;
               tmp = node->prev;
               node->prev = naf;
               write_buffer(p);

               read_buffer(naf);
               node->parent = p;
               node->prev = tmp;
               node->next = p;
               write_buffer(naf);

               if (tmp != END) {
                  read_buffer(tmp);
                  node->next = naf;
                  write_buffer(tmp);
               }
               else {
                  root.first = naf;
                  narf_sync();
               }

               break;
            }
         }
         else if (cmp > 0) {
            if (node->right != END) {
               p = node->right;
               ++height;
            }
            else {
               // we are the new right of p
               node->right = naf;
               tmp = node->next;
               node->next = naf;
               write_buffer(p);

               read_buffer(naf);
               node->parent = p;
               node->next = tmp;
               node->prev = p;
               write_buffer(naf);

               if (tmp != END) {
                  read_buffer(tmp);
                  node->prev = naf;
                  write_buffer(tmp);
               }
               else {
                  root.last = naf;
                  narf_sync();
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

   if (height > FORCE_REBALANCE) {
      narf_rebalance();
   }

   return true;
}

//! @see narf.h
bool narf_mkfs(uint32_t sectors) {
   if (!narf_io_open()) return false;

   memset(buffer, 0, sizeof(buffer));
   root.signature     = SIGNATURE;
   root.version       = VERSION;
   root.sector_size   = SECTOR_SIZE;
   root.total_sectors = sectors;
   root.vacant        = 1;
   root.root          = END;
   root.first         = END;
   root.last          = END;
   root.chain         = END;
   memcpy(buffer, &root, sizeof(root));
   write_buffer(0);

#ifdef NARF_DEBUG
   printf("keysize %ld\n", sizeof(node->key));
#endif

   return true;
}

//! @see narf.h
bool narf_init(void) {
   if (!narf_io_open()) return false;

   read_buffer(0);
   memcpy(&root, buffer, sizeof(root));

#ifdef NARF_DEBUG
   printf("keysize %ld\n", sizeof(node->key));
#endif

   return verify();
}

//! @see narf.h
bool narf_sync(void) {
   if (!verify()) return false;
   memset(buffer, 0, sizeof(buffer));
   memcpy(buffer, &root, sizeof(root));
   return write_buffer(0);
}

//! @see narf.h
NAF narf_find(const char *key) {
   NAF naf = root.root;
   int cmp;

   if (!verify()) return false;

   while(1) {
      if (naf == END) {
         return naf;
      }
      read_buffer(naf);
      cmp = strncmp(key, node->key, sizeof(node->key));
      if (cmp < 0) {
         naf = node->left;
      }
      else if (cmp > 0) {
         naf = node->right;
      }
      else {
         return naf;
      }
   }
   // TODO FIX detect endless loops???
}

//! @see narf.h
NAF narf_dirfirst(const char *dirname, const char *sep) {
   NAF naf;
   int cmp;

   if (!verify()) return false;

   if (root.root == END) return END;

   naf = root.root;

   while(1) {
      read_buffer(naf);
      cmp = strncmp(dirname, node->key, sizeof(node->key));
      if (cmp < 0) {
         if (node->left != END) {
            naf = node->left;
         }
         else {
            // the current node comes AFTER us
            naf = node->prev;
            break;
         }
      }
      else if (cmp > 0) {
         if (node->right != END) {
            naf = node->right;
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

//! @see narf.h
NAF narf_dirnext(const char *dirname, const char *sep, NAF naf) {
   uint32_t dirname_len;
   uint32_t sep_len;
   char *p;

   if (!verify()) return false;

   if (naf != END) {
      read_buffer(naf);
      naf = node->next;
   }
   else {
      naf = root.first;
   }

   if (naf == END) {
      return END;
   }

   read_buffer(naf);

   // at this point, "naf" is (probably) valid,
   // it is in the buffer,
   // and it is the first node after us.

   dirname_len = strlen(dirname);
   if (strncmp(dirname, node->key, dirname_len)) {
      // not a match at all!
      return END;
   }

   sep_len = strlen(sep);
   while (!strncmp(dirname, node->key, dirname_len)) {
      // beginning matches
      p = strstr(node->key + dirname_len, sep);
      if (p == NULL || p[sep_len] == 0) {
         // no sep, or only one sep at end
         return naf;
      }
      naf = node->next;
      if (naf != END) {
         read_buffer(naf);
      }
      else {
         break;
      }
   }

   return END;
}

//! @see narf.h
NAF narf_alloc(const char *key, uint32_t size) {
   NAF naf;
   uint32_t prev;
   uint32_t next;

   uint32_t length;
   uint32_t excess;

   length = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;

   if (!verify()) return END;

   naf = narf_find(key);

   if (naf != END) {
      return false;
   }

   // first check if we can allocate from the chain
   prev = END;
   next = root.chain;
   while(next != END) {
      read_buffer(next);
      if (node->length >= length) {
         // this will do nicely
         naf = next;
         next = node->next;

         // pull it out
         if (prev == END) {
            root.chain = next;
            narf_sync();
         }
         else {
            read_buffer(prev);
            node->next = next;
            write_buffer(prev);
         }

         read_buffer(naf);
         if (node->length > length) {
            // we need to trim the excess.
            excess = node->length - length;
            prev = node->start + length;

            node->length = length;
            write_buffer(naf);

            read_buffer(prev);
            node->start = prev + 1;
            node->length = excess - 1;
            write_buffer(prev);

            narf_chain(prev);

            read_buffer(naf);
         }
         break;
      }
      prev = next;
      next = node->next;
   }

   if (naf == END) {
      // nothing on the chain was suitable
      naf = root.vacant;
      ++root.vacant;
      node->start  = root.vacant;
      node->length = length;
      root.vacant += length;
   }

   // reset fields except start and length
   node->parent = END;
   node->left   = END;
   node->right  = END;
   node->prev    = END;
   node->next    = END;
   node->bytes  = size;
   strncpy(node->key, key, sizeof(node->key));
   write_buffer(naf);

#ifdef NARF_DEBUG
   printf("alloc %08x %08x %d %d\n",
         naf, node->start, node->length, node->bytes);
#endif

   narf_sync();
   narf_insert(naf, key);

   return naf;
}

//! @see narf.h
bool narf_free(const char *key) {
   NAF naf;
   NAF prev;
   NAF next;
#ifdef SMART_FREE
   NAF left;
   NAF right;
   NAF beta;
#endif

   if (!verify()) return false;

   naf = narf_find(key);

   if (naf == END) {
      return false;
   }

   // unlink from list
   read_buffer(naf);
   prev = node->prev;
   next = node->next;
#ifdef SMART_FREE
   left = node->left;
   right = node->right;
#endif
   node->prev = END;
   node->next = END;
   write_buffer(naf);

   if (next != END) {
      read_buffer(next);
      node->prev = prev;
      write_buffer(next);
   }
   else {
      root.last = prev;
      narf_sync();
   }

   if (prev != END) {
      read_buffer(prev);
      node->next = next;
      write_buffer(prev);
   }
   else {
      root.first = next;
      narf_sync();
   }

#ifdef SMART_FREE
   // remove from tree
   if (root.root == naf) {
      if (left == END && right == END) {
         root.root = END;
         goto sync_chain;
      }
      // TODO FIX this is similar to non-root case below
      else if (left == END) {
         read_buffer(right);
         node->parent = END;
         write_buffer(right);

         root.root = right;
         goto sync_chain;
      }
      else if (right == END) {
         read_buffer(left);
         node->parent = END;
         write_buffer(left);

         root.root = left;
         goto sync_chain;
      }
      else {
         // chose a random direction to pivot
         if (naf & 1) {
            read_buffer(left);
            beta = node->right;
            node->right = naf;
            node->parent = END;
            write_buffer(left);

            read_buffer(naf);
            node->left = beta;
            node->parent = left;
            write_buffer(naf);

            root.root = left;
         }
         else {
            read_buffer(right);
            beta = node->left;
            node->left = naf;
            node->parent = END;
            write_buffer(right);

            read_buffer(naf);
            node->right = beta;
            node->parent = right;
            write_buffer(naf);

            root.root = right;
         }
      }
   }

   // if we're here, we have a left and a right
   // and we're not root
   read_buffer(naf);
   prev = node->parent; // note reuse of variable
   left = node->left;
   right = node->right;

   while (left != END && right != END) {
      // wobble down the chain until we have a free sibling
      read_buffer(prev);

      // TODO FIX this is similar to root case above
      if (node->left == naf) {

         node->left = left;
         write_buffer(prev);

         read_buffer(left);
         beta = node->right;
         node->right = naf;
         node->parent = prev;
         write_buffer(left);

         read_buffer(naf);
         prev = node->parent = left;
         left = node->left = beta;
         write_buffer(naf);
      }
      else if (node->right == naf) {

         node->right = right;
         write_buffer(prev);

         read_buffer(right);
         beta = node->left;
         node->left = naf;
         node->parent = prev;
         write_buffer(right);

         read_buffer(naf);
         prev = node->parent = right;
         right = node->right = beta;
         write_buffer(naf);
      }
      else {
         // this should never happen
         // TODO FIX possible infinite loop here
         assert(0);
      }
   }

   // TODO FIX this is similar to root case above
   if (left == END) {
      if (right != END) {
         read_buffer(right);
         node->parent = prev;
         write_buffer(right);
      }
      beta = right; // note reuse of variable
   }
   else if (right == END) {
      read_buffer(left);
      node->parent = prev;
      write_buffer(left);
      beta = left; // note reuse of variable
   }
   else {
      // TODO FIX this should never happen
      assert(0);
   }

   read_buffer(prev);
   if (node->left == naf) {
      node->left = beta;
   }
   else if (node->right == naf) {
      node->right = beta;
   }
   else {
      // this should never happen
      assert(0);
   }
   write_buffer(prev);

sync_chain:
   narf_sync();
   narf_chain(naf);

   // TODO FIX call rebalance intelligently!

#else
   narf_sync();
   narf_chain(naf);
   narf_rebalance();
#endif // SMART_FREE

   return true;
}

//! @see narf.h
bool narf_rebalance(void) {
   static char key[sizeof(((Header *) 0)->key)]; // EXPENSIVE !!!
   uint32_t head = root.first;

   uint32_t sector = root.first;
   uint32_t count = 0;
   uint32_t target = 0;
   uint32_t spot = 0;

   uint32_t numerator;
   uint32_t denominator = 2;

   uint32_t prev;
   uint32_t next;

   if (!verify()) return false;

   while (sector != END) {
      ++count;
      read_buffer(sector);
      sector = node->next;
   }

   root.root = END;
   root.first = END;
   root.last = END;
   narf_sync();

   while (denominator < count) {
      // odd multiples of denominator
      sector = head;
      numerator = 1;
      target = count * numerator / denominator;
      spot = 0;

      while (numerator < denominator && sector != END) {
         read_buffer(sector);
         while (sector != END) {
            next = node->next;
            if (spot == target) {
               prev = node->prev;

               if (head == sector) {
                  head = next;
               }
               if (prev != END) {
                  read_buffer(prev);
                  node->next = next;
                  write_buffer(prev);
               }
               if (next != END) {
                  read_buffer(next);
                  node->prev = prev;
                  write_buffer(next);
               }

               read_buffer(sector);
               node->prev = END;
               node->next = END;
               node->left = END;
               node->right = END;
               node->parent = END;
               strncpy(key, node->key, sizeof(node->key));
               write_buffer(sector);

               narf_insert(sector, key);

               numerator += 2;
               target = count * numerator / denominator;
            }
            ++spot;
            sector = next;
            if (sector != END) {
               read_buffer(sector);
            }
         }
      }

      count = count - denominator / 2;
      denominator *= 2;
   }

   // now finish the job
   sector = head;
   while (sector != END) {
      read_buffer(sector);
      next = node->next;
      node->prev = END;
      node->next = END;
      node->left = END;
      node->right = END;
      node->parent = END;
      strncpy(key, node->key, sizeof(node->key));
      write_buffer(sector);

      narf_insert(sector, key);

      sector = next;
   }

   return true;
}

//! @see narf.h
const char *narf_key(NAF naf) {
   if (!verify() || naf == END) return NULL;
   read_buffer(naf);
   return node->key;
}

//! @see narf.h
uint32_t narf_sector(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->start;
}

//! @see narf.h
uint32_t narf_size(NAF naf) {
   if (!verify() || naf == END) return 0;
   read_buffer(naf);
   return node->bytes;
}

//! @see narf.h
NAF narf_first(void) {
   if (!verify()) return END;
   return root.first;
}

//! @see narf.h
NAF narf_next(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->next;
}

//! @see narf.h
NAF narf_last(void) {
   if (!verify()) return END;
   return root.last;
}

//! @see narf.h
NAF narf_previous(NAF naf) {
   if (!verify() || naf == END) return END;
   read_buffer(naf);
   return node->prev;
}

#ifdef NARF_DEBUG
//! @brief Helper used to pretty print the NARF tree.
//!
//! @param sector The current sector being printed
//! @param indent The number of levels to indent
//! @param pattern Bitfield indicating tree limbs to print
static void narf_pt(uint32_t sector, int indent, uint32_t pattern) {
   uint32_t l, r;
   int i;
   char *p;
   char c;

   if (!verify()) return;

   if (sector != END) {
      read_buffer(sector);
      l = node->left;
      r = node->right;
      p = strdup(node->key);
      narf_pt(l, indent + 1, pattern);
   }

   for (i = 0; i < indent; i++) {
      if (pattern & (1 << i)) {
         printf("|  ");
      }
      else {
         printf("   ");
      }
   }

   if (indent) {
      if (pattern & (1 << indent)) {
         c = '\\';
      }
      else {
      c = '/';
      }
   }
   else {
      c = '-';
   }

   if (sector == END) {
      printf("%c- (nil)\n", c);
      return;
   }
   else {
      printf("%c- %s [%d]\n", c, p, sector);
      free(p);
   }

   narf_pt(r, indent + 1, (pattern ^ (3 << (indent))) & ~1);
}

//! see narf.h
void narf_debug(void) {
   uint32_t sector;

   printf("root.signature     = %08x '%4s'\n", root.signature, root.sigbytes);
   if (root.signature != SIGNATURE) {
      printf("bad signature\n");
      return;
   }

   printf("root.version       = %08x\n", root.version);
   if (root.version != VERSION) {
      printf("bad version\n");
      return;
   }

   printf("root.sector_size   = %d\n", root.sector_size);
   if (root.sector_size != SECTOR_SIZE) {
      printf("bad sector size\n");
      return;
   }

   printf("root.total_sectors = %d\n", root.total_sectors);
   if (root.total_sectors < 2) {
      printf("bad total sectors\n");
      return;
   }

   printf("root.vacant        = %d\n", root.vacant);
   printf("root.chain         = %d\n", root.chain);
   printf("root.root          = %d\n", root.root);
   printf("root.first         = %d\n", root.first);

   sector = root.first;
   while (sector != END) {
      printf("\n");
      read_buffer(sector);
      printf("sector = %d\n", sector);
      printf("key    = '%s'\n", node->key);
      printf("parent = %d\n", node->parent);
      printf("left   = %d\n", node->left);
      printf("right  = %d\n", node->right);
      printf("prev   = %d\n", node->prev);
      printf("next   = %d\n", node->next);
      printf("start  = %d\n", node->start);
      printf("length = %d\n", node->length);
      printf("bytes  = %d\n", node->bytes);

      sector = node->next;
   }

   printf("\nfreechain:\n");
   sector = root.chain;
   while (sector != END) {
      read_buffer(sector);
      printf("%d (%d:%d) -> %d\n", sector, node->start, node->length, node->next);
      sector = node->next;
   }

   narf_pt(root.root, 0, 0);
}
#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
