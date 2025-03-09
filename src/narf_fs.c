#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf_io.h"
#include "narf_fs.h"
#include "narf_data.h"

#ifdef NARF_DEBUG
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#endif

#define SECTOR_SIZE 512

uint8_t buffer[SECTOR_SIZE] = { 0 };
NARF_Root root = { 0 };
NARF_Header *node = (NARF_Header *) buffer;

static bool read_buffer(uint32_t sector) {
   return narf_io_read(sector, buffer);
}

static bool write_buffer(uint32_t sector) {
   return narf_io_write(sector, buffer);
}

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
   write_buffer(0);
   return true;
}

/// Initialize a NARF
///
/// @return true for success
bool narf_init(void) {
   read_buffer(0);
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
   return write_buffer(0);
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
      read_buffer(ret);
      cmp = strncmp(key, node->key, sizeof(node->key));
      if (cmp < 0) {
         ret = node->left;
      }
      else if (cmp > 0) {
         ret = node->right;
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
      read_buffer(ret);
      cmp = strncmp(key, node->key, strlen(key));
      if (cmp < 0) {
         ret = node->left;
      }
      else if (cmp > 0) {
         ret = node->right;
      }
      else {
         prev = node->prev;
         while (prev != NARF_END) {
            read_buffer(prev);
            if (strncmp(key, node->key, strlen(key))) {
               break;
            }
            ret = prev;
            prev = node->prev;
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
         read_buffer(p);
         cmp = strncmp(key, node->key, sizeof(node->key));
         if (cmp < 0) {
            if (node->left != NARF_END) {
               p = node->left;
            }
            else {
               node->left = sector;
               tmp = node->prev;
               node->prev = sector;
               write_buffer(p);

               read_buffer(sector);
               node->parent = p;
               node->prev = tmp;
               node->next = p;
               write_buffer(sector);

               if (tmp != NARF_END) {
                  read_buffer(tmp);
                  node->next = sector;
                  write_buffer(tmp);
               }
               else {
                  root.first = sector;
                  narf_sync();
               }

               break;
            }
         }
         else if (cmp > 0) {
            if (node->right != NARF_END) {
               p = node->right;
            }
            else {
               node->right = sector;
               tmp = node->next;
               node->next = sector;
               write_buffer(p);

               read_buffer(sector);
               node->parent = p;
               node->next = tmp;
               node->prev = p;
               write_buffer(sector);

               if (tmp != NARF_END) {
                  read_buffer(tmp);
                  node->prev = sector;
                  write_buffer(tmp);
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

/// add a sector to the free chain
///
/// @param sector The sector to add
void narf_chain(uint32_t sector) {
   uint32_t prev;
   uint32_t next;
   uint32_t length;

   // reset fields
   read_buffer(sector);
   node->prev = NARF_END;
   node->next = NARF_END;
   node->left = NARF_END;
   node->right = NARF_END;
   node->parent = NARF_END;
   node->bytes = 0;
   // do NOT reset "start" and "length"
   write_buffer(sector);

   // record them in the free chain
   if (root.chain == NARF_END) {
      // done above // read_buffer(sector);
      node->next = root.chain;
      write_buffer(sector);

      root.chain = sector;
      narf_sync();
   }
   else {
      // smallest records first
      // done above // read_buffer(sector);
      length = node->length;

      prev = NARF_END;
      next = root.chain;
      read_buffer(next);

      while (length > node->length && next != NARF_END) {
         prev = next;
         next = node->next;
         if (next != NARF_END) {
            read_buffer(next);
         }
      }
      if (prev == NARF_END) {
         root.chain = sector;
         narf_sync();
      }
      else {
         read_buffer(prev);
         node->next = sector;
         write_buffer(prev);
      }
      read_buffer(sector);
      node->next = next;
      write_buffer(sector);
   }
}

/// Allocate storage for key
///
/// @param key The key we're allocating for
/// @return The new sector
uint32_t narf_alloc(const char *key, uint32_t size) {
   uint32_t s;
   uint32_t prev;
   uint32_t next;

   uint32_t length;
   uint32_t excess;

   length = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;

   if (!verify()) return false;

   s = narf_find(key);

   if (s != NARF_END) {
      return false;
   }

   // first check if we can allocate from the chain
   prev = NARF_END;
   next = root.chain;
   while(next != NARF_END) {
      read_buffer(next);
      if (node->length >= length) {
         // this will do nicely
         s = next;
         next = node->next;

         // pull it out
         if (prev == NARF_END) {
            root.chain = next;
            narf_sync();
         }
         else {
            read_buffer(prev);
            node->next = next;
            write_buffer(prev);
         }

         read_buffer(s);
         if (node->length > length) {
            // we need to trim the excess.
            excess = node->length - length;
            prev = node->start + length;

            node->length = length;
            write_buffer(s);

            read_buffer(prev);
            node->start = prev + 1;
            node->length = excess - 1;
            write_buffer(prev);

            narf_chain(prev);

            read_buffer(s);
         }
         break;
      }
      prev = next;
      next = node->next;
   }

   if (s == NARF_END) {
      // nothing on the chain was suitable
      s = root.vacant;
      ++root.vacant;
      node->start  = root.vacant;
      node->length = length;
      root.vacant += length;
   }

   // reset fields except start and length
   node->parent = NARF_END;
   node->left   = NARF_END;
   node->right  = NARF_END;
   node->prev    = NARF_END;
   node->next    = NARF_END;
   node->bytes  = size;
   strncpy(node->key, key, sizeof(node->key));
   write_buffer(s);

#ifdef NARF_DEBUG
   printf("alloc %08x %08x %d %d\n",
         s, node->start, node->length, node->bytes);
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

   read_buffer(sector);
   prev = node->prev;
   next = node->next;
   write_buffer(sector);

   if (next != NARF_END) {
      read_buffer(next);
      node->prev = prev;
      write_buffer(next);
   }

   if (prev != NARF_END) {
      read_buffer(prev);
      node->next = next;
      write_buffer(prev);
   }
   else {
      root.first = next;
      narf_sync();
   }

   narf_chain(sector);

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
      read_buffer(sector);
      sector = node->next;
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
         read_buffer(sector);
         while (sector != NARF_END) {
            next = node->next;
            if (spot == target) {
               prev = node->prev;

               if (head == sector) {
                  head = next;
               }
               if (prev != NARF_END) {
                  read_buffer(prev);
                  node->next = next;
                  write_buffer(prev);
               }
               if (next != NARF_END) {
                  read_buffer(next);
                  node->prev = prev;
                  write_buffer(next);
               }

               read_buffer(sector);
               node->prev = NARF_END;
               node->next = NARF_END;
               node->left = NARF_END;
               node->right = NARF_END;
               node->parent = NARF_END;
               strncpy(key, node->key, sizeof(node->key));
               write_buffer(sector);

               narf_insert(sector, key);

               numerator += 2;
               target = count * numerator / denominator;
            }
            ++spot;
            sector = next;
            if (sector != NARF_END) {
               read_buffer(sector);
            }
         }
      }

      count = count - denominator / 2;
      denominator *= 2;
   }

   // now finish the job
   sector = head;
   while (sector != NARF_END) {
      read_buffer(sector);
      next = node->next;
      node->prev = NARF_END;
      node->next = NARF_END;
      node->left = NARF_END;
      node->right = NARF_END;
      node->parent = NARF_END;
      strncpy(key, node->key, sizeof(node->key));
      write_buffer(sector);

      narf_insert(sector, key);

      sector = next;
   }

   return true;
}

#ifdef NARF_DEBUG
static void narf_pt(uint32_t sector, int indent, uint32_t pattern) {
   uint32_t l, r;
   int i;
   char *p;

   if (!verify()) return;

   if (sector != NARF_END) {
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

   if (sector == NARF_END) {
      printf("+- (nil)\n");
      return;
   }
   else {
      printf("+- %s [%d]\n", p, sector);
      free(p);
   }

   narf_pt(r, indent + 1, (pattern ^ (3 << (indent))) & ~1);
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
   while (sector != NARF_END) {
      read_buffer(sector);
      printf("%d (%d:%d) -> %d\n", sector, node->start, node->length, node->next);
      sector = node->next;
   }

   narf_pt(root.root, 0, 0);
}
#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
