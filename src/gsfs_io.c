#include <stdio.h>
#include <sys/mman.h>

#include "gsfs_io.h"

// This is an example implementation using mmap'd files.

#define SECTOR_SIZE 512
#define BYTES (1024*1024*1024) // 1 Gig
#define SECTORS (BYTES / SECTOR_SIZE)

static uint8_t *image;

bool gsfs_io_open(void) {
   return true;
}

bool gsfs_io_close(void) {
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
