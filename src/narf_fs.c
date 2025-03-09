#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf_io.h"
#include "narf_fs.h"
#include "narf_data.h"

#define DEBUG
#include <stdio.h>

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

/// Free storage for key
///
/// @param key The key we're freeing
/// @return true for success
bool narf_free(const char *key);

/// Rebalance the entire tree
///
/// @return true for success
bool narf_rebalance(void);

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
               narf_io_write(p, buffer);

               narf_io_read(sector, buffer);
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

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
