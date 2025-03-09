#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf_io.h"
#include "narf_fs.h"
#include "narf_data.h"

uint8_t buffer[512];

bool narf_format(void) {
   GSFS_RootNode *p = (GSFS_RootNode *)buffer;
   memset(buffer, 0, sizeof(buffer));
   p->head.signature = GSFS_SIGNATURE;
   p->head.version   = GSFS_VERSION;
   p->head.node_type = GSFS_NODE_ROOT;
   p->directory_tree = GSFS_TAIL;
   p->free_list      = GSFS_TAIL;
   p->size           = 1;

   return narf_io_write(0, buffer);
}

bool narf_check_head(void) {
   GSFS_NodeHeader *head = (GSFS_NodeHeader *)buffer;
   return head->signature == GSFS_SIGNATURE &&
          head->version == GSFS_VERSION;
}

bool narf_mkdir(uint32_t sector, const char *name) {
   GSFS_RootNode *p = (GSFS_RootNode *)buffer;
again:
   if (!narf_io_read(sector, buffer)) return false;
   if (!narf_check_head()) return false;

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
