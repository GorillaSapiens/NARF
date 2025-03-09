#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "narf_io.h"
#include "narf_fs.h"
#include "narf_data.h"

uint8_t buffer[512];

bool narf_format(void) {
   NARF_RootNode *p = (NARF_RootNode *)buffer;
   memset(buffer, 0, sizeof(buffer));
   p->head.signature = NARF_SIGNATURE;
   p->head.version   = NARF_VERSION;
   p->head.node_type = NARF_NODE_ROOT;
   p->directory_tree = NARF_TAIL;
   p->free_list      = NARF_TAIL;
   p->size           = 1;

   return narf_io_write(0, buffer);
}

bool narf_check_head(void) {
   NARF_NodeHeader *head = (NARF_NodeHeader *)buffer;
   return head->signature == NARF_SIGNATURE &&
          head->version == NARF_VERSION;
}

bool narf_mkdir(uint32_t sector, const char *name) {
   NARF_RootNode *p = (NARF_RootNode *)buffer;
again:
   if (!narf_io_read(sector, buffer)) return false;
   if (!narf_check_head()) return false;

   if (p->head.node_type == NARF_NODE_ROOT) {
      if
   }
   else if (p->head.node_type == NARF_NODE_DIR) {
   }
   else {
      return false;
   }

   return true;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
