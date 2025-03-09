#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf_io.h"
#include "narf_fs.h"
#include "narf_data.h"

#define DEBUG
#include <stdio.h>
#include <math.h>

#define SECTOR_SIZE 512

uint8_t buffer[SECTOR_SIZE];
NARF_Root root;
NARF_Header *header = (NARF_Header *) buffer;

/// Create a NARF
///
/// @return true for success
bool narf_mkfs(void) {
   memset(buffer, 0, sizeof(buffer));
   root.signature   = NARF_SIGNATURE;
   root.version     = NARF_VERSION;
   root.sector_size = SECTOR_SIZE;
   root.free        = 1;
   root.root        = NARF_TAIL;
   root.first       = NARF_TAIL;
   memcpy(buffer, &root, sizeof(root));
   narf_io_write(0, buffer);
   return true;
}

/// Initialize a NARF
///
/// @return true for success
bool narf_init(void) {
   narf_io_read(0, buffer);
   memcpy(&root, buffer, sizeof(root));
   if (root.signature != NARF_SIGNATURE) {
      return false;
   }
   if (root.version != NARF_VERSION) {
      return false;
   }
   if (root.sector_size != SECTOR_SIZE) {
      return false;
   }

   return true;
}

/// Sync the narf to disk
///
/// @return true for success
bool narf_sync(void) {
   if (root.signature != NARF_SIGNATURE) {
      return false;
   }
   if (root.version != NARF_VERSION) {
      return false;
   }
   if (root.sector_size != SECTOR_SIZE) {
      return false;
   }

   memset(buffer, 0, sizeof(buffer));
   memcpy(buffer, &root, sizeof(root));
   return narf_io_write(0, buffer);
}

/// Find the sector number matching the key
///
/// @param key The key to look for
/// @return The sector of the key, or NARF_TAIL if not found
uint32_t narf_find(const char *key) {
   uint32_t ret = root.root;
   int cmp;

   while(1) {
      if (ret == NARF_TAIL) {
         return ret;
      }
      narf_io_read(ret, buffer);
      cmp = strncmp(key, header->key, sizeof(header->key));
      if (cmp < 0) {
         ret = header->left;
      }
      else if (cmp > 0) {
         ret = header->right;
      }
      else {
         return ret;
      }
   }
   // TODO FIX detect endless loops???
}

/// Find the sector number matching the key substring
///
/// @param key The key to look for
/// @return The sector of the key, or NARF_TAIL if not found
uint32_t narf_dirfind(const char *key);

/// Allocate storage for key
///
/// @param key The key we're allocating for
/// @return The new sector
uint32_t narf_alloc(const char *key, uint32_t size) {
   uint32_t s = narf_find(key);

   if (s != NARF_TAIL) {
      return false;
   }

   s = root.free;
   ++root.free;

   header->parent = NARF_TAIL;
   header->left   = NARF_TAIL;
   header->right  = NARF_TAIL;
   header->prv    = NARF_TAIL;
   header->nxt    = NARF_TAIL;
   header->start  = root.free;
   header->length = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
   header->bytes  = size;

   root.free += header->length;

   strncpy(header->key, key, sizeof(header->key));
   narf_io_write(s, buffer);

#ifdef DEBUG
   printf("alloc %08x %08x %d %d\n",
         s, header->start, header->length, header->bytes);
#endif

   narf_sync();
   narf_insert(s, key);
}

bool narf_pivotdown(uint32_t sector) {
   uint32_t parent;
   uint32_t right;
   uint32_t beta;

   narf_io_read(sector, buffer);
   while (header->left != NARF_TAIL && header->right != NARF_TAIL) {

      parent = header->parent;
      right = header->right;
      narf_io_read(right, buffer);
      beta = header->left;

      if (parent == NARF_TAIL) {
         // we were root
         root.root = right;
         narf_sync();
      }
      else {
         narf_io_read(parent, buffer);
         if (header->left == sector) {
            header->left = right;
         }
         else if (header->right == sector) {
            header->right = right;
         }
         else {
            // this should never happen
         }
         narf_io_write(parent, buffer);
      }

      narf_io_read(right, buffer);
      header->parent = parent;
      header->left = sector;
      narf_io_write(right, buffer);

      if (beta != NARF_TAIL) {
         narf_io_read(beta, buffer);
         header->parent = sector;
         narf_io_write(beta, buffer);
      }

      // must be last
      narf_io_read(sector, buffer);
      header->parent = right;
      header->right = beta;
      narf_io_write(sector, buffer);
   }
}

/// Free storage for key
///
/// @param key The key we're freeing
/// @return true for success
bool narf_free(const char *key) {
   uint32_t sector = narf_find(key);
   uint32_t prv;
   uint32_t nxt;

   if (sector == NARF_TAIL) {
      return false;
   }

   narf_io_read(sector, buffer);
   prv = header->prv;
   nxt = header->nxt;
   header->prv = NARF_TAIL;
   header->nxt = NARF_TAIL;
   header->left = NARF_TAIL;
   header->right = NARF_TAIL;
   header->parent = NARF_TAIL;
   header->bytes = 0;
   narf_io_write(sector, buffer);

   if (nxt != NARF_TAIL) {
      narf_io_read(nxt, buffer);
      header->prv = prv;
      narf_io_write(nxt, buffer);
   }

   if (prv != NARF_TAIL) {
      narf_io_read(prv, buffer);
      header->nxt = nxt;
      narf_io_write(prv, buffer);
   }
   else {
      root.first = nxt;
      narf_sync();
   }

   // TODO store them, and the data, in a free chain

   narf_rebalance();

   return true;
}

/// Rebalance the entire tree
///
/// EXPENSIVE !!!
///
/// @return true for success
bool narf_rebalance(void) {
   static uint8_t key[SECTOR_SIZE]; // EXPENSIVE !!!
   uint32_t head = root.first;

   uint32_t sector = root.first;
   uint32_t count = 0;
   uint32_t target = 0;
   uint32_t spot = 0;

   uint32_t numerator;
   uint32_t denominator = 2;

   uint32_t prv;
   uint32_t nxt;

   while (sector != NARF_TAIL) {
      ++count;
      narf_io_read(sector, buffer);
      sector = header->nxt;
   }

   root.root = NARF_TAIL;
   root.first = NARF_TAIL;
   narf_sync();

   while (denominator < count) {
      // odd multiples of denominator
      sector = head;
      numerator = 1;
      target = count * numerator / denominator;
      spot = 0;

      while (numerator < denominator) {
         narf_io_read(sector, buffer);
         while (sector != NARF_TAIL) {
            nxt = header->nxt;
            if (spot == target) {
               prv = header->prv;

               if (head == sector) {
                  head = nxt;
               }
               if (prv != NARF_TAIL) {
                  narf_io_read(prv, buffer);
                  header->nxt = nxt;
                  narf_io_write(prv, buffer);
               }
               if (nxt != NARF_TAIL) {
                  narf_io_read(nxt, buffer);
                  header->prv = prv;
                  narf_io_write(nxt, buffer);
               }

               narf_io_read(sector, buffer);
               header->prv = NARF_TAIL;
               header->nxt = NARF_TAIL;
               header->left = NARF_TAIL;
               header->right = NARF_TAIL;
               header->parent = NARF_TAIL;
               strncpy(key, header->key, sizeof(header->key));
               narf_io_write(sector, buffer);

               narf_insert(sector, key);

               numerator += 2;
               target = count * numerator / denominator;
            }
            ++spot;
            sector = nxt;
            if (sector != NARF_TAIL) {
               narf_io_read(sector, buffer);
            }
         }
      }

      count = count - denominator / 2;
      denominator *= 2;
   }

   // now finish the job
   sector = head;
   while (sector != NARF_TAIL) {
      narf_io_read(sector, buffer);
      nxt = header->nxt;
      header->prv = NARF_TAIL;
      header->nxt = NARF_TAIL;
      header->left = NARF_TAIL;
      header->right = NARF_TAIL;
      header->parent = NARF_TAIL;
      strncpy(key, header->key, sizeof(header->key));
      narf_io_write(sector, buffer);

      narf_insert(sector, key);

      sector = nxt;
   }

   return true;
}

/// insert sector into the tree
///
/// @return true for success
bool narf_insert(uint32_t sector, const uint8_t *key) {
   uint32_t tmp;
   uint32_t nxt;
   uint32_t p;
   int cmp;
   if (root.root == NARF_TAIL) {
      root.root = sector;
      root.first = sector;
      narf_sync();
   }
   else {
      p = root.root;
      while (1) {
         narf_io_read(p, buffer);
         cmp = strncmp(key, header->key, sizeof(header->key));
         if (cmp < 0) {
            if (header->left != NARF_TAIL) {
               p = header->left;
            }
            else {
               header->left = sector;
               tmp = header->prv;
               header->prv = sector;
               narf_io_write(p, buffer);

               narf_io_read(sector, buffer);
               header->parent = p;
               header->prv = tmp;
               header->nxt = p;
               narf_io_write(sector, buffer);

               if (tmp != NARF_TAIL) {
                  narf_io_read(tmp, buffer);
                  header->nxt = sector;
                  narf_io_write(tmp, buffer);
               }
               else {
                  root.first = sector;
                  narf_sync();
               }

               break;
            }
         }
         else if (cmp > 0) {
            if (header->right != NARF_TAIL) {
               p = header->right;
            }
            else {
               header->right = sector;
               tmp = header->nxt;
               header->nxt = sector;
               narf_io_write(p, buffer);

               narf_io_read(sector, buffer);
               header->parent = p;
               header->nxt = tmp;
               header->prv = p;
               narf_io_write(sector, buffer);

               if (tmp != NARF_TAIL) {
                  narf_io_read(tmp, buffer);
                  header->prv = sector;
                  narf_io_write(tmp, buffer);
               }

               break;
            }
         }
         else {
            // this should never happen !!!
         }
      }
   }
   return true;
}

void narf_pt(uint32_t sector, int indent) {
   uint32_t l, r;
   char *p;
   if (sector == NARF_TAIL) {
      printf("%*s\n", indent * 2, "-");
      return;
   }
   narf_io_read(sector, buffer);
   l = header->left;
   r = header->right;
   p = strdup(header->key);
   narf_pt(l, indent + 1);
   printf("%*s\n", indent * 2, p);
   narf_pt(r, indent + 1);
}

void narf_debug(void) {
   uint32_t sector;

   printf("root.signature   = %08x\n", root.signature);
   printf("root.version     = %08x\n", root.version);
   printf("root.sector_size = %d\n", root.sector_size);
   printf("root.free        = %d\n", root.free);
   printf("root.root        = %d\n", root.root);
   printf("root.first       = %d\n", root.first);

   sector = root.first;

   while (sector != NARF_TAIL) {
      printf("\n");
      narf_io_read(sector, buffer);
      printf("sector = %d\n", sector);
      printf("key    = '%s'\n", header->key);
      printf("left   = %d\n", header->left);
      printf("right  = %d\n", header->right);
      printf("prv    = %d\n", header->prv);
      printf("nxt    = %d\n", header->nxt);
      printf("start  = %d\n", header->start);
      printf("length = %d\n", header->length);
      printf("bytes  = %d\n", header->bytes);

      sector = header->nxt;
   }

   narf_pt(root.root, 0);
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
