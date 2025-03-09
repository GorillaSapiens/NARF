#include "gsfs_io.h"

// This is an example implementation using mmap'd files.

bool gsfs_io_open(void) {
   return true;
}

bool gsfs_io_close(void) {
   return true;
}

bool gsfs_io_write(uint32_t sector, uint8_t *data) {
   return true;
}

bool gsfs_io_read(uint32_t sector, uint8_t *data) {
   return true;
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
