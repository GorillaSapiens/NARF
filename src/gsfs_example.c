#include <stdio.h>
#include <string.h>

#include "gsfs_io.h"
#include "gsfs_fs.h"

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
         printf("gsfs_format()=%d\n", result ASSIGN gsfs_format());
      }
      else {
         printf("huh?\n");
      }

      printf("#>");
   }
}

int main(int argc, char **argv) {
   bool result;

   printf("GSFS example\n");

   printf("gsfs_io_open()=%d\n", result ASSIGN gsfs_io_open());

   if (result) {
      printf("gsfs_io_sectors()=%08X\n", gsfs_io_sectors());

      loop();

      printf("gsfs_io_close()=%d\n", result ASSIGN gsfs_io_close());
   }
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
