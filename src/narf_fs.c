#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf_io.h"
#include "narf_fs.h"
#include "narf_data.h"

#ifdef NARF_DEBUG
#include <stdio.h>
#include <math.h>
#endif

#define SECTOR_SIZE 512

uint8_t buffer[SECTOR_SIZE] = { 0 };
NARF_Root root = { 0 };
NARF_Header *header = (NARF_Header *) buffer;

static bool verify(void) {
   if (root.signature != NARF_SIGNATURE) return false;
   if (root.version != NARF_VERSION) return false;
   if (root.sector_size != SECTOR_SIZE) return false;
   return true;
}

/// Create a NARF
///
/// @return true for success
bool narf_mkfs(uint32_t sectors) {
   memset(buffer, 0, sizeof(buffer));
   root.signature     = NARF_SIGNATURE;
   root.version       = NARF_VERSION;
   root.sector_size   = SECTOR_SIZE;
   root.total_sectors = sectors;
   root.vacant        = 1;
   root.root          = NARF_END;
   root.first         = NARF_END;
   root.chain         = NARF_END;
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
   return verify();
}

/// Sync the narf to disk
///
/// @return true for success
bool narf_sync(void) {
   if (!verify()) return false;
   memset(buffer, 0, sizeof(buffer));
   memcpy(buffer, &root, sizeof(root));
   return narf_io_write(0, buffer);
}

/// Find the sector number matching the key
///
/// @param key The key to look for
/// @return The sector of the key, or NARF_END if not found
uint32_t narf_find(const char *key) {
   uint32_t ret = root.root;
   int cmp;

   if (!verify()) return false;

   while(1) {
      if (ret == NARF_END) {
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
/// @return The sector of the key, or NARF_END if not found
uint32_t narf_dirfind(const char *key) {
   uint32_t ret = root.root;
   uint32_t prev;
   int cmp;

   if (!verify()) return false;

   while(1) {
      if (ret == NARF_END) {
         return ret;
      }
      narf_io_read(ret, buffer);
      cmp = strncmp(key, header->key, strlen(key));
      if (cmp < 0) {
         ret = header->left;
      }
      else if (cmp > 0) {
         ret = header->right;
      }
      else {
         prev = header->prev;
         while (prev != NARF_END) {
            narf_io_read(prev, buffer);
            if (strncmp(key, header->key, strlen(key))) {
               break;
            }
            ret = prev;
            prev = header->prev;
         }
         return ret;
      }
   }
   // TODO FIX detect endless loops???
}

/// insert sector into the tree
///
/// @return true for success
static bool narf_insert(uint32_t sector, const uint8_t *key) {
   uint32_t tmp;
   uint32_t next;
   uint32_t p;
   int cmp;

   if (!verify()) return false;

   if (root.root == NARF_END) {
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
            if (header->left != NARF_END) {
               p = header->left;
            }
            else {
               header->left = sector;
               tmp = header->prev;
               header->prev = sector;
               narf_io_write(p, buffer);

               narf_io_read(sector, buffer);
               header->parent = p;
               header->prev = tmp;
               header->next = p;
               narf_io_write(sector, buffer);

               if (tmp != NARF_END) {
                  narf_io_read(tmp, buffer);
                  header->next = sector;
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
            if (header->right != NARF_END) {
               p = header->right;
            }
            else {
               header->right = sector;
               tmp = header->next;
               header->next = sector;
               narf_io_write(p, buffer);

               narf_io_read(sector, buffer);
               header->parent = p;
               header->next = tmp;
               header->prev = p;
               narf_io_write(sector, buffer);

               if (tmp != NARF_END) {
                  narf_io_read(tmp, buffer);
                  header->prev = sector;
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

/// Allocate storage for key
///
/// @param key The key we're allocating for
/// @return The new sector
uint32_t narf_alloc(const char *key, uint32_t size) {
   uint32_t s;

   if (!verify()) return false;

   s = narf_find(key);

   if (s != NARF_END) {
      return false;
   }

   s = root.vacant;
   ++root.vacant;

   header->parent = NARF_END;
   header->left   = NARF_END;
   header->right  = NARF_END;
   header->prev    = NARF_END;
   header->next    = NARF_END;
   header->start  = root.vacant;
   header->length = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
   header->bytes  = size;

   root.vacant += header->length;

   strncpy(header->key, key, sizeof(header->key));
   narf_io_write(s, buffer);

#ifdef NARF_DEBUG
   printf("alloc %08x %08x %d %d\n",
         s, header->start, header->length, header->bytes);
#endif

   narf_sync();
   narf_insert(s, key);
}

/// Free storage for key
///
/// @param key The key we're freeing
/// @return true for success
bool narf_free(const char *key) {
   uint32_t sector;
   uint32_t prev;
   uint32_t next;
   uint32_t length;

   if (!verify()) return false;

   sector = narf_find(key);

   if (sector == NARF_END) {
      return false;
   }

   narf_io_read(sector, buffer);
   prev = header->prev;
   next = header->next;
   header->prev = NARF_END;
   header->next = NARF_END;
   header->left = NARF_END;
   header->right = NARF_END;
   header->parent = NARF_END;
   header->bytes = 0;
   narf_io_write(sector, buffer);

   if (next != NARF_END) {
      narf_io_read(next, buffer);
      header->prev = prev;
      narf_io_write(next, buffer);
   }

   if (prev != NARF_END) {
      narf_io_read(prev, buffer);
      header->next = next;
      narf_io_write(prev, buffer);
   }
   else {
      root.first = next;
      narf_sync();
   }

   // record them in the free chain
   if (root.chain == NARF_END) {
      narf_io_read(sector, buffer);
      header->next = root.chain;
      narf_io_write(sector, buffer);
      root.chain = sector;
      narf_sync();
   }
   else {
      // smallest records first
      narf_io_read(sector, buffer);
      length = header->length;

      // reuse of prev and next variables here
      prev = NARF_END;
      next = root.chain;
      narf_io_read(next, buffer);

      while (length > header->length && next != NARF_END) {
         prev = next;
         next = header->next;
         if (next != NARF_END) {
            narf_io_read(next, buffer);
         }
      }
      if (prev == NARF_END) {
         root.chain = sector;
         narf_sync();
      }
      else {
         narf_io_read(prev, buffer);
         header->next = sector;
         narf_io_write(prev, buffer);
      }
      narf_io_read(sector, buffer);
      header->next = next;
      narf_io_write(sector, buffer);
   }

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

   uint32_t prev;
   uint32_t next;

   if (!verify()) return false;

   while (sector != NARF_END) {
      ++count;
      narf_io_read(sector, buffer);
      sector = header->next;
   }

   root.root = NARF_END;
   root.first = NARF_END;
   narf_sync();

   while (denominator < count) {
      // odd multiples of denominator
      sector = head;
      numerator = 1;
      target = count * numerator / denominator;
      spot = 0;

      while (numerator < denominator) {
         narf_io_read(sector, buffer);
         while (sector != NARF_END) {
            next = header->next;
            if (spot == target) {
               prev = header->prev;

               if (head == sector) {
                  head = next;
               }
               if (prev != NARF_END) {
                  narf_io_read(prev, buffer);
                  header->next = next;
                  narf_io_write(prev, buffer);
               }
               if (next != NARF_END) {
                  narf_io_read(next, buffer);
                  header->prev = prev;
                  narf_io_write(next, buffer);
               }

               narf_io_read(sector, buffer);
               header->prev = NARF_END;
               header->next = NARF_END;
               header->left = NARF_END;
               header->right = NARF_END;
               header->parent = NARF_END;
               strncpy(key, header->key, sizeof(header->key));
               narf_io_write(sector, buffer);

               narf_insert(sector, key);

               numerator += 2;
               target = count * numerator / denominator;
            }
            ++spot;
            sector = next;
            if (sector != NARF_END) {
               narf_io_read(sector, buffer);
            }
         }
      }

      count = count - denominator / 2;
      denominator *= 2;
   }

   // now finish the job
   sector = head;
   while (sector != NARF_END) {
      narf_io_read(sector, buffer);
      next = header->next;
      header->prev = NARF_END;
      header->next = NARF_END;
      header->left = NARF_END;
      header->right = NARF_END;
      header->parent = NARF_END;
      strncpy(key, header->key, sizeof(header->key));
      narf_io_write(sector, buffer);

      narf_insert(sector, key);

      sector = next;
   }

   return true;
}

#ifdef NARF_DEBUG
static void narf_pt(uint32_t sector, int indent) {
   uint32_t l, r;
   char *p;

   if (!verify()) return;

   if (sector == NARF_END) {
      printf("%*s\n", indent * 2 + 1, "-");
      return;
   }
   narf_io_read(sector, buffer);
   l = header->left;
   r = header->right;
   p = strdup(header->key);
   narf_pt(l, indent + 1);
   printf("%*s\n", indent * 2 + strlen(p), p);
   narf_pt(r, indent + 1);
}

void narf_debug(void) {
   uint32_t sector;

   printf("root.signature     = %08x '%4s'\n", root.signature, root.sigbytes);
   if (root.signature != NARF_SIGNATURE) {
      printf("bad signature\n");
      return;
   }

   printf("root.version       = %08x\n", root.version);
   if (root.version != NARF_VERSION) {
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

   while (sector != NARF_END) {
      printf("\n");
      narf_io_read(sector, buffer);
      printf("sector = %d\n", sector);
      printf("key    = '%s'\n", header->key);
      printf("left   = %d\n", header->left);
      printf("right  = %d\n", header->right);
      printf("prev   = %d\n", header->prev);
      printf("next   = %d\n", header->next);
      printf("start  = %d\n", header->start);
      printf("length = %d\n", header->length);
      printf("bytes  = %d\n", header->bytes);

      sector = header->next;
   }

   narf_pt(root.root, 0);
}
#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
