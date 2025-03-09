#include <stdio.h>
#include <string.h>

#include "narf_io.h"
#include "narf_fs.h"

#define ASSIGN =

const char *tf[] = { "false", "true" };

void loop(void) {
   char buffer[1024];
   bool result;

   printf("#>");
   while(gets(buffer)) {

      if (!strncmp(buffer, "exit", 4)) {
         break;
      }
      else if (!strncmp(buffer, "quit", 4)) {
         break;
      }
      else if (!strncmp(buffer, "mkfs", 4)) {
         uint32_t sectors = narf_io_sectors();
         printf("narf_mkfs(0x%x)=%s\n",
            sectors, tf[result ASSIGN narf_mkfs(sectors)]);
      }
      else if (!strncmp(buffer, "init", 4)) {
         printf("narf_init()=%s\n",
            tf[result ASSIGN narf_init()]);
      }
      else if (!strncmp(buffer, "rebalance", 9)) {
         printf("narf_rebalance()=%s\n",
            tf[result ASSIGN narf_rebalance()]);
      }
#ifdef NARF_DEBUG
      else if (!strncmp(buffer, "debug", 5)) {
         narf_debug();
      }
#endif
      else if (!strncmp(buffer, "sync", 4)) {
         printf("narf_sync()=%s\n",
            tf[result ASSIGN narf_sync()]);
      }
      else if (!strncmp(buffer, "alloc ", 6)) {
         char key[256];
         int size;
         sscanf(buffer, "alloc %s %d", key, &size);
         printf("narf_alloc(%s,%d)=%d\n",
            key, size, result ASSIGN narf_alloc(key, size));
      }
      else if (!strncmp(buffer, "free ", 5)) {
         char key[256];
         sscanf(buffer, "free %s", key);
         printf("narf_free(%s)=%s\n",
            key, tf[result ASSIGN narf_free(key)]);
      }
      else if (!strncmp(buffer, "ls ", 3)) {
         char key[256];
         sscanf(buffer, "ls %s", key);
         printf("narf_dirfind(%s)=%d\n", key, narf_dirfind(key));
      }
      else if (!strncmp(buffer, "cat ", 4)) {
         char key[256];
         sscanf(buffer, "cat %s", key);
         printf("narf_find(%s)=%d\n", key, narf_find(key));
      }
      else {
         printf("huh?\n");
      }

      printf("#>");
   }
}

int main(int argc, char **argv) {
   bool result;

   printf("NARF example\n");

   printf("narf_io_open()=%d\n", result ASSIGN narf_io_open());

   if (result) {
      printf("narf_io_sectors()=%08X\n", narf_io_sectors());

      loop();

      printf("narf_io_close()=%d\n", result ASSIGN narf_io_close());
   }
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
