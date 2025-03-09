#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#include "gsfs_io.h"

// This is an example implementation using mmap'd files.

#define FILENAME "example.gsfs"
#define SECTOR_SIZE 512
#define BYTES (1024*1024*1024) // 1 Gig
#define SECTORS (BYTES / SECTOR_SIZE)

static uint8_t *image;
static int fd;

bool gsfs_io_open(void) {
   if (access(FILENAME, F_OK) == 0) {
      fd = open(FILENAME, O_RDWR, 0);
      if (fd == -1) {
         perror("open example.gsfs");
         exit(-1);
      }
   }
   else {
      fd = open(FILENAME, O_RDWR | O_CREAT, 0666);
      if (fd == -1) {
         perror("create example.gsfs");
         exit(-1);
      }
   }
   image = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (image == NULL) {
      perror("mmap");
      exit(-1);
   }

   return true;
}

bool gsfs_io_close(void) {
   if (munmap(image, BYTES) == -1) {
      perror("munmap");
      exit(-1);
   }
   close(fd);
   return true;
}

uint32_t gsfs_io_sectors(void) {
   return SECTORS;
}

bool gsfs_io_write(uint32_t sector, uint8_t *data) {
   return true;
}

bool gsfs_io_read(uint32_t sector, uint8_t *data) {
   return true;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
