#ifndef _INCLUDE_NARF_DATA_H_
#define _INCLUDE_NARF_DATA_H_

#include <assert.h>
#include <stdint.h>

#define NARF_SIGNATURE 0x4652414E // FRAN
#define NARF_VERSION 0x00000000

#define NARF_TAIL 0xFFFFFFFF

typedef struct __attribute__((packed)) {
   uint32_t signature;   // NARF_SIGNATURE
   uint32_t version;     // NARF_VERSION
   uint32_t sector_size; // sector size in bytes
   uint32_t free;        // number of first free sector
   uint32_t root;        // sector of root node
   uint32_t first;       // sector of root node
} NARF_Root;
static_assert(sizeof(NARF_Root) == 24, "NARF_Root wrong size");

typedef struct __attribute__((packed)) {
   uint32_t left;        // left sibling sector
   uint32_t right;       // right sibling sector
   uint32_t prv;         // next sequential sector
   uint32_t nxt;         // next sequential sector

   uint32_t start;       // the starting sector
   uint32_t length;      // the length in sectors
   uint32_t bytes;       // size in bytes

   uint8_t key[512 - 7 * sizeof(uint32_t)]; // key
} NARF_Header;

static_assert(sizeof(NARF_Header) == 512, "NARF_Header wrong size");

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
