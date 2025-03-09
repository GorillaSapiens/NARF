#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "gsfs_io.h"
#include "gsfs_fs.h"
#include "gsfs_data.h"

uint8_t buffer[512];

bool gsfs_format(void) {
   GSFS_RootNode *p = (GSFS_RootNode *)buffer;
   memset(buffer, 0, sizeof(buffer));
   p->head.signature = GSFS_SIGNATURE;
   p->head.version   = GSFS_VERSION;
   p->head.node_type = GSFS_NODE_ROOT;
   p->directory_tree = GSFS_TAIL;
   p->free_list      = GSFS_TAIL;
   p->size           = 1;

   return gsfs_io_write(0, buffer);
}

bool gsfs_check_head(void) {
   GSFS_NodeHeader *head = (GSFS_NodeHeader *)buffer;
   return head->signature == GSFS_SIGNATURE &&
          head->version == GSFS_VERSION;
}

bool gsfs_mkdir(uint32_t sector, const char *name) {
   GSFS_RootNode *p = (GSFS_RootNode *)buffer;
again:
   if (!gsfs_io_read(sector, buffer)) return false;
   if (!gsfs_check_head()) return false;

   if (p->head.node_type == GSFS_NODE_ROOT) {
      if
   }
   else if (p->head.node_type == GSFS_NODE_DIR) {
   }
   else {
      return false;
   }

   return true;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
