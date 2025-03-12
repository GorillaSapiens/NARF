#include <stdio.h>
#include <string.h>

#include "narf.h"
#include "narf_io.h"

#define ASSIGN =

const char *tf[] = { "false", "true" };

void loop(void) {
   char buffer[1024];

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
         bool result;

         printf("narf_mkfs(0x%x)=%s\n",
            sectors, tf[result ASSIGN narf_mkfs(sectors)]);
      }
      else if (!strncmp(buffer, "init", 4)) {
         bool result;

         printf("narf_init()=%s\n",
            tf[result ASSIGN narf_init()]);
      }
      else if (!strncmp(buffer, "rebalance", 9)) {
         bool result;

         printf("narf_rebalance()=%s\n",
            tf[result ASSIGN narf_rebalance()]);
      }
#ifdef NARF_DEBUG
      else if (!strncmp(buffer, "debug", 5)) {
         narf_debug();
      }
#endif
      else if (!strncmp(buffer, "sync", 4)) {
         bool result;

         printf("narf_sync()=%s\n",
            tf[result ASSIGN narf_sync()]);
      }
      else if (!strncmp(buffer, "alloc ", 6)) {
         char key[256];
         int size;
         NAF result;

         sscanf(buffer, "alloc %s %d", key, &size);
         printf("narf_alloc(%s,%d)=%d\n",
            key, size, result ASSIGN narf_alloc(key, size));
      }
      else if (!strncmp(buffer, "realloc ", 8)) {
         char key[256];
         int size;
         NAF result;

         sscanf(buffer, "realloc %s %d", key, &size);
         printf("narf_realloc(%s,%d)=%d\n",
            key, size, result ASSIGN narf_realloc(key, size));
      }
      else if (!strncmp(buffer, "slurp ",6)) {
         char p[512];
         FILE *f;
         NAF result;

         sscanf(buffer, "slurp %s", p);
         f = fopen(p, "r");

         if (f) {
            while (fgets(p, sizeof(p), f)) {
               p[strlen(p) - 1] = 0;
               printf("narf_alloc(%s,%d)=%d\n",
                  p, 1024, result ASSIGN narf_alloc(p, 1024));
            }
            fclose(f);
         }
      }
      else if (!strncmp(buffer, "free ", 5)) {
         char key[256];
         bool result;

         sscanf(buffer, "free %s", key);
         printf("narf_free(%s)=%s\n",
            key, tf[result ASSIGN narf_free(key)]);
      }
      else if (!strncmp(buffer, "ls ", 3)) {
         uint32_t sector;
         char key[256];
         sscanf(buffer, "ls %s", key);

         printf("\n");
         for (sector = narf_dirfirst(key, "/");
              sector != -1;
              sector = narf_dirnext(key, "/", sector)) {
            printf("%d %s\n", sector, narf_key(sector));
         }
         printf("\n");
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

      // always sync() before closing the io layer
      printf("narf_sync()=%d\n", result ASSIGN narf_sync());

      printf("narf_io_close()=%d\n", result ASSIGN narf_io_close());
   }
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
