#include <stdio.h>
#include <string.h>

#include "narf_io.h"
#include "narf_fs.h"

#define ASSIGN =

void loop(void) {
   char buffer[1024];
   bool result;

   printf("#>");
   while(gets(buffer)) {

      if (!strncmp(buffer, "exit", 4)) {
         break;
      }
      else if (!strncmp(buffer, "mkfs", 4)) {
         printf("narf_format()=%d\n", result ASSIGN narf_format());
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
