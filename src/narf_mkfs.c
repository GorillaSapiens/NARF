#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "narf_conf.h"
#include "narf_io.h"
#include "narf.h"

int         fd               = -1;
off_t       size             = -1;
const char *target           = NULL;
int         write_mbr        = 0;
int         format           = 0;
int         partition_number = -1;

//! @brief Initialize the mkfs image I/O layer.
//!
//! @return true on success.
bool narf_io_open(void) {
   return true;
}

//! @brief Deinitialize the mkfs image I/O layer.
//!
//! @return true on success.
bool narf_io_close(void) {
   return true;
}

//! @brief Get the mkfs target image size in sectors.
//!
//! @return the number of sectors supported by the device
uint32_t narf_io_sectors(void) {
   return size / NARF_SECTOR_SIZE;
}

//! @brief Write one sector to the mkfs target image.
//!
//! @param sector Sector address to access.
//! @param data Pointer to one sector of data to write.
//! @return true on success.
bool narf_io_write(uint32_t sector, void *data) {
   off_t offset;
   ssize_t written;

   if (data == NULL) {
      return false;
   }

   if (sector >= narf_io_sectors()) {
      return false;
   }

   offset = (off_t) sector * NARF_SECTOR_SIZE;

   if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
      return false;
   }

   written = write(fd, data, NARF_SECTOR_SIZE);

   if (written != NARF_SECTOR_SIZE) {
      return false;
   }

   fsync(fd);
   return true;
}

//! @brief Read one sector from the mkfs target image.
//!
//! @param sector Sector address to access.
//! @param data Pointer to one sector of read buffer.
//! @return true on success.
bool narf_io_read(uint32_t sector, void *data) {
   off_t offset;
   ssize_t bytes;

   if (data == NULL) {
      return false;
   }

   if (sector >= narf_io_sectors()) {
      return false;
   }

   offset = (off_t) sector * NARF_SECTOR_SIZE;

   if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
      return false;
   }

   bytes = read(fd, data, NARF_SECTOR_SIZE);

   if (bytes != NARF_SECTOR_SIZE) {
      return false;
   }

   return true;
}

//! @brief Print narf_mkfs usage help.
void usage(const char *progname) {
   fprintf(stderr, "Usage: %s <size>[K|M|G] <target.img> [mbr] [format] [part=N]\n", progname);
   fprintf(stderr, "       %s <target.img> [mbr] [format] [part=N]    (if file already exists)\n", progname);
   exit(1);
}

//! @brief Parse a size string with optional K/M/G suffix.
off_t parsesize(const char *arg) {
   char *endptr;
   off_t size = strtoull(arg, &endptr, 10);
   if (size <= 0) return -1;

   switch (*endptr) {
      case 'K': case 'k': return size * 1024;
      case 'M': case 'm': return size * 1024 * 1024;
      case 'G': case 'g': return size * 1024 * 1024 * 1024;
      case '\0':          return size;
      default:            return -1;
   }
}

//! @brief Create or open the target image file.
int create_open_file(void){
   struct stat st;

   if (size >= 0) {
      // Creation mode: file must not already exist.
      if (access(target, F_OK) == 0) {
         fprintf(stderr, "Error: file '%s' already exists\n", target);
         return 1;
      }

      fd = open(target, O_CREAT | O_EXCL | O_RDWR, 0644);
      if (fd < 0) {
         perror("open");
         return 1;
      }

      if (ftruncate(fd, size) != 0) {
         perror("ftruncate");
         close(fd);
         return 1;
      }

      printf("Created '%s' with size %lld bytes\n", target, (long long)size);
   }
   else {
      // Existing-image mode: file must already exist.
      fd = open(target, O_RDWR);
      if (fd < 0) {
         perror("open existing");
         return 1;
      }

      if (fstat(fd, &st) != 0) {
         perror("fstat");
         close(fd);
         return 1;
      }

      if (st.st_size <= 0) {
         fprintf(stderr, "Error: file '%s' has invalid size\n", target);
         close(fd);
         return 1;
      }

      size = st.st_size;

      printf("Opened existing '%s'\n", target);
   }
   return 0;
}

//! @brief Report an operation failure and exit with failure status.
int fail(const char *mesg) {
   fprintf(stderr, "ERROR: %s\n", mesg);
   exit(-1);
}

//! @brief Parse arguments and format the requested NARF image.
int main(int argc, char *argv[]) {
   int argi = 1;

   if (argc < 2)
      usage(argv[0]);

   // Check whether the first argument is a size or a filename.
   if (access(argv[1], F_OK) != 0) {
      if (argc < 3)
         usage(argv[0]);

      size = parsesize(argv[1]);
      if (size < 0) {
         fprintf(stderr, "Invalid size: %s\n", argv[1]);
         return 1;
      }

      target = argv[2];
      argi = 3;
   }
   else {
      target = argv[1];
      argi = 2;
   }

   for (int i = argi; i < argc; ++i) {
      if (strcmp(argv[i], "mbr") == 0) {
         write_mbr = 1;
      }
      else if (strcmp(argv[i], "format") == 0) {
         format = 1;
      }
      else if (strncmp(argv[i], "part=", 5) == 0) {
         partition_number = atoi(argv[i] + 5);
         if (partition_number < 1 || partition_number > 4) {
            fprintf(stderr, "Invalid partition number: %s\n", argv[i]);
            return 1;
         }
      }
      else {
         usage(argv[0]);
      }
   }

   printf("Target: %s\n", target);
   if (size >= 0) {
      printf("Size: %lld\n", (long long)size);
   }
   else {
      printf("Size: (not specified, target exists)\n");
   }

   printf("Write MBR: %s\n", write_mbr ? "yes" : "no");
   printf("Format   : %s\n", format ? "yes" : "no");
   printf("Partition: %d\n", partition_number);

   if (create_open_file()) {
      return 1;
   }

   if (write_mbr) {
      if (!narf_mbr(NULL)) fail("narf_mbr() fail");
   }

   if (partition_number != -1) {
      if (!narf_partition(partition_number)) fail("narf_partition() fail");

      if (format) {
         if (!narf_format(partition_number)) fail("narf_format() fail");
      }

      if (!narf_mount(partition_number)) fail("narf_mount() fail");
   }
   else {
      if (format) {
         if (!narf_mkfs(0, size / NARF_SECTOR_SIZE)) fail("narf_mkfs() fail");
      }

      if (!narf_init(0)) fail("narf_init() fail");
   }

   return 0;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
