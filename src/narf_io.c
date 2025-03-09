#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#include "narf_io.h"

// This is an example implementation using mmap'd files.

#define FILENAME "example.narf"
#define SECTOR_SIZE 512

static uint8_t *image = NULL;
static int fd = -1;
static size_t total_bytes = 0;

bool narf_io_open(void) {

   errno = 0;
   if (access(FILENAME, F_OK) != 0) {
      if (system("dd if=/dev/zero of=example.narf bs=1K count=1M")) {
         if (errno) {
            perror("system dd [...]");
         }
         fprintf(stderr, "could not create NARF\n");
         return false;
      }
   }

   errno = 0;
   struct stat st;
   if (stat("example.narf", &st) == 0) {
      total_bytes = st.st_size;
   }
   else {
      if (errno) {
         perror("stat");
      }
      fprintf(stderr, "could not stat NARF\n");
      return false;
   }

   errno = 0;
   fd = open(FILENAME, O_RDWR, 0);
   if (fd == -1) {
      perror("open example.narf");
      return false;
   }

   errno = 0;
   image = mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (image == NULL) {
      perror("mmap");
      return false;
   }

   return true;
}

bool narf_io_close(void) {
   bool ret = true;

   sync();
   if (munmap(image, total_bytes) == -1) {
      perror("munmap");
      ret = false;
   }
   if (close(fd)) {
      perror("close");
      ret = false;
   }
   return ret;
}

uint32_t narf_io_sectors(void) {
   return total_bytes / SECTOR_SIZE;
}

bool narf_io_write(uint32_t sector, uint8_t *data) {
   bool ret = true;
   if (NULL == memcpy(image + sector * SECTOR_SIZE, data, SECTOR_SIZE)) {
      ret = false;
   }
   return ret;
}

bool narf_io_read(uint32_t sector, uint8_t *data) {
   bool ret = true;
   if (NULL == memcpy(data, image + sector * SECTOR_SIZE, SECTOR_SIZE)) {
      ret = false;
   }
   return ret;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
