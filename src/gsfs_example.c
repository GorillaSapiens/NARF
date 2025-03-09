#include <stdio.h>

#include "gsfs_io.h"

#define ASSIGN =

int main(int argc, char **argv) {
   bool result;

   printf("foo\n");

   printf("gsfs_io_open()=%d\n", result ASSIGN gsfs_io_open());

   if (result) {
      printf("gsfs_io_sectors()=%08X\n", gsfs_io_sectors());
      printf("gsfs_io_close()=%d\n", result ASSIGN gsfs_io_close());
   }
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
