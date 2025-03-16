#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "narf.h"
#include "narf_io.h"

#define ASSIGN =

extern void narf_io_configure(const char *fname);

const char *tf[] = { "false", "true" };

void gremlins(int s, int n);

void process_cmd(char *buffer) {
   if (!strncmp(buffer, "mkfs", 4)) {
      uint32_t start = 0;
      uint32_t size = narf_io_sectors();
      bool result;

      printf("narf_mkfs(0x%x, 0x%x)=%s\n",
            start, size, tf[result ASSIGN narf_mkfs(start, size)]);
   }
   else if (!strncmp(buffer, "init", 4)) {
      uint32_t start = 0;
      printf("narf_init(0x%x)=%s\n",
            start, tf[narf_init(start)]);
   }
   else if (!strncmp(buffer, "mbr", 3)) {
      if (strlen(buffer) > 4) {
         printf("narf_mbr(%s)=%s\n",
               buffer + 4, tf[narf_mbr(buffer + 4)]);
      }
      else {
         printf("narf_mbr(NULL)=%s\n",
               tf[narf_mbr(NULL)]);
      }
   }
   else if (!strncmp(buffer, "partition ", 10)) {
      int part = atoi(buffer + 10);
      printf("narf_partition(%d)=%s\n",
            part, tf[narf_partition(part)]);
   }
   else if (!strncmp(buffer, "findpart", 8)) {
      printf("narf_findpart()=%d\n",
            narf_findpart());
   }
   else if (!strncmp(buffer, "format ", 7)) {
      int part = atoi(buffer + 7);
      printf("narf_format(%d)=%s\n",
            part, tf[narf_format(part)]);
   }
   else if (!strncmp(buffer, "mount ", 6)) {
      int part = atoi(buffer + 6);
      printf("narf_mount(%d)=%s\n",
            part, tf[narf_mount(part)]);
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
   else if (!strncmp(buffer, "defrag", 6)) {
      bool result;

      printf("narf_defrag()=%s\n",
            tf[result ASSIGN narf_defrag()]);
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
   else if (!strncmp(buffer, "tag ", 4)) {
      char key[256];
      char data[32] = { 0 };
      bool result;

      sscanf(buffer, "tag %s %s", key, data);
      NAF naf = narf_find(key);
      printf("narf_set_metadata(%s,%s)=%s\n",
            key, data, tf[result ASSIGN narf_set_metadata(naf, (uint8_t *)data)]);
   }
   else if (!strncmp(buffer, "scan ", 5)) {
      char key[256];
      char *result;

      sscanf(buffer, "scan %s", key);
      NAF naf = narf_find(key);
      result = (char *)narf_metadata(naf);
      printf("narf_metadata(%s)=%s\n",
            key, result ? result : "(null)");
   }
   else if (!strncmp(buffer, "gremlins ", 9)) {
      int s, n;
      sscanf(buffer, "gremlins %d %d", &s, &n);
      gremlins(s, n);
   }
   else {
      printf("huh?\n");
   }
}

char *rname(int l) {
   static char buf[16];
   char *p;
   for (p = buf; p != buf + l; p++) {
      *p = 0x61 + rand() % 26;
   }
   *p = 0;
   return buf;
}

void gremlins(int s, int n) {
   char buf[1024];
   int m;
   int l;

   printf("gremlins %d %d\n", s, n);

   srand(s);

   l = rand() % 7 + 1; // length of keys

   process_cmd("mbr");
   process_cmd("partition 1");
   process_cmd("format 1");
   process_cmd("mount 1");

   for(m = 0; m < n; m++) {
      switch(rand() % 4) { // TODO FIX make rebalance / defrag infrequent
         case 0:
            sprintf(buf, "alloc %s %d", rname(l), rand() % 65536);
            break;
         case 1:
            sprintf(buf, "free %s", rname(l));
            break;
         case 2:
            sprintf(buf, "realloc %s %d", rname(l), rand() % 65536);
            break;
         case 3:
            sprintf(buf, "cat %s", rname(l));
            break;
         case 4:
            sprintf(buf, "rebalance");
            break;
         case 5:
            sprintf(buf, "defrag");
            break;
      }
      printf("\n\nGREMLINS %d: %s\n", m, buf);
      process_cmd(buf);
      printf("\nAFTER:\n");
      narf_debug();
      printf("\n");
   }

   //narf_debug();
   //narf_io_close();
   //exit(0);
}

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
      else {
         process_cmd(buffer);
      }
      printf("#>");
   }
}

int main(int argc, char **argv) {
   bool result;

   printf("NARF example\n");

   if (argc != 2) {
      fprintf(stderr, "Usage: %s <filename|=size,filename>\n", argv[0]);
      exit(0);
   }
   narf_io_configure(argv[1]);

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
