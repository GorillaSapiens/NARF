#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ctype.h>

#include "narf_io.h"

// This is an example implementation using mmap'd files.
// Substitute your own implementation for your hardware.

#define SECTOR_SIZE 512

static const char *filename = NULL;
static uint8_t *image = NULL;
static int fd = -1;
static size_t total_bytes = 0;

const char *strichr(const char *s, int c) {
   while (*s) {
      if (*s == c || *s == (c ^ 0x20)) { // ASSUMES ASCII !!!
         return s;
      }
      s++;
   }
   return NULL;
}

//! @brief Configure this narf_io_ implementation
//!
//! Used by outside
void narf_io_configure(const char *file) {
   const char *p;

   filename = file;
   if (*file == '=') {
      p = strchr(file, ',');
      if (p) {
         filename = p + 1;
         p--;
      }
      else {
         fprintf(stderr, "bad format for '=' filename: %s\n", file);
         exit(-1);
      }

      total_bytes = atoi(file + 1);

      if (p) {
         if (tolower(*p) == 'k') {
            total_bytes <<= 10;
         }
         else if (tolower(*p) == 'm') {
            total_bytes <<= 20;
         }
         else if (tolower(*p) == 'g') {
            total_bytes <<= 30;
         }
      }

      printf("total_bytes = %ld\n", total_bytes);
   }
}

//! @see narf_io.h
bool narf_io_open(void) {
   // if we're already open, just return true
   if (fd != -1) {
      return true;
   }

   errno = 0;
   if (access(filename, F_OK) != 0) {
      if (total_bytes == 0) {
         fprintf(stderr, "file '%s' does not exist.\n", filename);
         fprintf(stderr, "try '=16K,%s' to create a 16K file.\n", filename);
         return false;
      }

      int tmpfd = open(filename, O_CREAT | O_EXCL | O_RDWR, 0644);
      if (tmpfd < 0) {
         perror("open");
         return false;
      }

      if (ftruncate(tmpfd, total_bytes) != 0) {
         perror("ftruncate");
         close(tmpfd);
         return false;
      }

      close(tmpfd);
   }

   errno = 0;
   struct stat st;
   if (stat(filename, &st) == 0) {
      if (total_bytes && (total_bytes != (size_t) st.st_size)) {
         fprintf(stderr, "'%s' exists but is wrong size %ld vs %ld\n",
            filename, total_bytes, st.st_size);
         return false;
      }
      total_bytes = st.st_size;
   }
   else {
      if (errno) {
         perror("stat");
      }
      fprintf(stderr, "could not stat '%s'\n", filename);
      return false;
   }

   errno = 0;
   fd = open(filename, O_RDWR, 0);
   if (fd == -1) {
      fprintf(stderr, "open '%s'\n", filename);
      perror("open:");
      return false;
   }

   errno = 0;
   image = mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (image == MAP_FAILED) {
      perror("mmap");
      close(fd);
      fd = -1;
      image = NULL;
      return false;
   }

   return true;
}

//! @see narf_io.h
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
   sync();

   // reset file scope static variables
   image = NULL;
   fd = -1;
   total_bytes = 0;

   return ret;
}

//! @see narf_io.h
uint32_t narf_io_sectors(void) {
   return total_bytes / SECTOR_SIZE;
}

//! sanity checking
static bool sector_is_valid(uint32_t sector) {
   if (image == NULL) {
      return false;
   }

   if (sector >= narf_io_sectors()) {
      return false;
   }

   return true;
}

//! @see narf_io.h
bool narf_io_write(uint32_t sector, void *data) {
   size_t offset;

   if (data == NULL) {
      return false;
   }

   if (!sector_is_valid(sector)) {
      return false;
   }

   offset = (size_t) sector * SECTOR_SIZE;

   memcpy(image + offset, data, SECTOR_SIZE);
   return true;
}

//! @see narf_io.h
bool narf_io_read(uint32_t sector, void *data) {
   size_t offset;

   if (data == NULL) {
      return false;
   }

   if (!sector_is_valid(sector)) {
      return false;
   }

   offset = (size_t) sector * SECTOR_SIZE;

   memcpy(data, image + offset, SECTOR_SIZE);
   return true;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
