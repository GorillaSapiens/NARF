#define _FILE_OFFSET_BITS 64

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "narf_io.h"

// This is an example implementation using one 512-byte pread()/pwrite()
// transfer per NARF sector.  Substitute your own implementation for your
// hardware.

#define SECTOR_SIZE 512u

static const char *filename = NULL;
static int fd = -1;
static uint64_t total_bytes = 0;

const char *strichr(const char *s, int c) {
   while (*s) {
      if (*s == c || *s == (c ^ 0x20)) { // ASSUMES ASCII !!!
         return s;
      }
      s++;
   }
   return NULL;
}

//! @brief Parse a tester size specification of the form =16M,file.img.
static bool parse_create_size(const char *file, uint64_t *bytes, const char **name) {
   char *end;
   uint64_t value;
   uint64_t multiplier = 1;

   if (file == NULL || bytes == NULL || name == NULL) return false;
   if (*file != '=') return false;

   errno = 0;
   value = strtoull(file + 1, &end, 0);
   if (end == file + 1 || errno != 0) return false;

   if (*end == 'k' || *end == 'K') {
      multiplier = 1024ull;
      end++;
   }
   else if (*end == 'm' || *end == 'M') {
      multiplier = 1024ull * 1024ull;
      end++;
   }
   else if (*end == 'g' || *end == 'G') {
      multiplier = 1024ull * 1024ull * 1024ull;
      end++;
   }

   if (*end != ',') return false;
   if (end[1] == 0) return false;
   if (value > UINT64_MAX / multiplier) return false;

   *bytes = value * multiplier;
   *name = end + 1;
   return true;
}

//! @brief Return true when an image byte count is a supported sector count.
static bool byte_count_is_supported(uint64_t bytes) {
   if ((bytes % SECTOR_SIZE) != 0) return false;
   if ((bytes / SECTOR_SIZE) > UINT32_MAX) return false;
   return true;
}

//! @brief Configure this example narf_io implementation.
void narf_io_configure(const char *file) {
   filename = file;
   total_bytes = 0;

   if (file != NULL && *file == '=') {
      if (!parse_create_size(file, &total_bytes, &filename)) {
         fprintf(stderr, "bad format for '=' filename: %s\n", file);
         exit(-1);
      }

      if (!byte_count_is_supported(total_bytes)) {
         fprintf(stderr, "bad image size: %" PRIu64 " bytes\n", total_bytes);
         fprintf(stderr, "image size must be a whole number of 512-byte sectors and fit in uint32_t sectors.\n");
         exit(-1);
      }

      printf("total_bytes = %" PRIu64 "\n", total_bytes);
   }
}

//! @see narf_io.h
bool narf_io_open(void) {
   struct stat st;

   // If already open, succeed without reopening.
   if (fd != -1) {
      return true;
   }

   if (filename == NULL) {
      fprintf(stderr, "no filename configured\n");
      return false;
   }

   errno = 0;
   if (access(filename, F_OK) != 0) {
      int tmpfd;

      if (total_bytes == 0) {
         fprintf(stderr, "file '%s' does not exist.\n", filename);
         fprintf(stderr, "try '=16K,%s' to create a 16K file.\n", filename);
         return false;
      }

      tmpfd = open(filename, O_CREAT | O_EXCL | O_RDWR, 0644);
      if (tmpfd < 0) {
         perror("open");
         return false;
      }

      if (ftruncate(tmpfd, (off_t) total_bytes) != 0) {
         perror("ftruncate");
         close(tmpfd);
         return false;
      }

      if (close(tmpfd) != 0) {
         perror("close");
         return false;
      }
   }

   errno = 0;
   if (stat(filename, &st) == 0) {
      if (st.st_size < 0) {
         fprintf(stderr, "'%s' has a negative size?\n", filename);
         return false;
      }

      if (total_bytes && (total_bytes != (uint64_t) st.st_size)) {
         fprintf(stderr, "'%s' exists but is wrong size %" PRIu64 " vs %" PRIu64 "\n",
            filename, total_bytes, (uint64_t) st.st_size);
         return false;
      }
      total_bytes = (uint64_t) st.st_size;
   }
   else {
      if (errno) {
         perror("stat");
      }
      fprintf(stderr, "could not stat '%s'\n", filename);
      return false;
   }

   if (!byte_count_is_supported(total_bytes)) {
      fprintf(stderr, "bad image size: %" PRIu64 " bytes\n", total_bytes);
      return false;
   }

   errno = 0;
   fd = open(filename, O_RDWR, 0);
   if (fd == -1) {
      fprintf(stderr, "open '%s'\n", filename);
      perror("open:");
      return false;
   }

   return true;
}

//! @see narf_io.h
bool narf_io_close(void) {
   bool ret = true;

   if (fd != -1) {
      if (fsync(fd) == -1) {
         perror("fsync");
         ret = false;
      }
      if (close(fd) != 0) {
         perror("close");
         ret = false;
      }
   }

   // reset file scope static variables
   fd = -1;
   total_bytes = 0;

   return ret;
}

//! @see narf_io.h
uint32_t narf_io_sectors(void) {
   return (uint32_t)(total_bytes / SECTOR_SIZE);
}

//! @brief Return true when a sector is inside the opened image.
static bool sector_is_valid(uint32_t sector) {
   if (fd == -1) {
      return false;
   }

   if (sector >= narf_io_sectors()) {
      return false;
   }

   return true;
}

//! @brief Do one exact-size positional file transfer.
static bool exact_pio(bool write_op, uint32_t sector, void *data) {
   uint8_t *p = (uint8_t *) data;
   size_t done = 0;
   off_t offset;

   if (data == NULL) return false;
   if (!sector_is_valid(sector)) return false;

   offset = (off_t) sector * (off_t) SECTOR_SIZE;

   while (done < SECTOR_SIZE) {
      ssize_t n;

      if (write_op) {
         n = pwrite(fd, p + done, SECTOR_SIZE - done, offset + (off_t) done);
      }
      else {
         n = pread(fd, p + done, SECTOR_SIZE - done, offset + (off_t) done);
      }

      if (n < 0) {
         if (errno == EINTR) continue;
         return false;
      }

      if (n == 0) {
         return false;
      }

      done += (size_t) n;
   }

   return true;
}

//! @see narf_io.h
bool narf_io_write(uint32_t sector, void *data) {
   return exact_pio(true, sector, data);
}

//! @see narf_io.h
bool narf_io_read(uint32_t sector, void *data) {
   return exact_pio(false, sector, data);
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
