#ifndef _INCLUDE_NARF_DATA_H_
#define _INCLUDE_NARF_DATA_H_

#include <assert.h>
#include <stdint.h>

#define NARF_SIGNATURE 0x4652414E // FRAN
#define NARF_VERSION 0x00000000

#define NARF_END 0xFFFFFFFF

typedef struct __attribute__((packed)) {
   union {
      uint32_t signature;  // NARF_SIGNATURE
      uint8_t sigbytes[4];
   };
   uint32_t version;       // NARF_VERSION
   uint32_t sector_size;   // sector size in bytes
   uint32_t total_sectors; // total size of storage in sectors
   uint32_t root;          // sector of root node
   uint32_t first;         // sector of root node
   uint32_t chain;         // previously allocated but free now
   uint32_t vacant;        // number of first unallocated
} NARF_Root;
static_assert(sizeof(NARF_Root) == 8 * sizeof(uint32_t), "NARF_Root wrong size");

typedef struct __attribute__((packed)) {
   uint32_t parent;      // parent sector
   uint32_t left;        // left sibling sector
   uint32_t right;       // right sibling sector
   uint32_t prev;        // previous ordered sector
   uint32_t next;        // next ordered sector

   uint32_t start;       // data start sector
   uint32_t length;      // data length in sectors
   uint32_t bytes;       // data size in bytes

   uint8_t key[512 - 8 * sizeof(uint32_t)]; // key
} NARF_Header;

static_assert(sizeof(NARF_Header) == 512, "NARF_Header wrong size");

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
