#ifndef _INCLUDE_NARF_CONF_H_
#define _INCLUDE_NARF_CONF_H_

// Tunable parameters go in this file

// Uncomment this for debugging functions
#define NARF_DEBUG

// Uncomment this for debugging structure integrity
// Beware, this makes EVERYTHING very slow!
//#define NARF_DEBUG_INTEGRITY

// Size of a sector in bytes
// beware of large values, NARF keeps a sector sized
// buffer in memory.
#define NARF_SECTOR_SIZE 512u

// Minimum virgin metadata sectors kept aside for COW deletes/GC.
// Normal allocations may not consume this reserve; recovery-style
// transactions such as free/defrag may.  This prevents a full medium from
// becoming unable to delete keys.
#ifndef NARF_METADATA_RESERVE_SECTORS
#define NARF_METADATA_RESERVE_SECTORS 32
#endif

// Number of bits in a sector address
// NB: currently only 32 is actually supported !!!
#define NARF_SECTOR_ADDRESS_BITS 32

// DO NOT TOUCH THIS BLOCK
#if NARF_SECTOR_ADDRESS_BITS != 32
#error "Only 32-bit NARF sector addresses are currently supported"
#endif

// DO NOT TOUCH THIS BLOCK
#if NARF_SECTOR_ADDRESS_BITS == 8
   #define NARF_SECTOR_ADDRESS_TYPE uint8_t
#elif NARF_SECTOR_ADDRESS_BITS == 16
   #define NARF_SECTOR_ADDRESS_TYPE uint16_t
#elif NARF_SECTOR_ADDRESS_BITS == 32
   #define NARF_SECTOR_ADDRESS_TYPE uint32_t
#elif NARF_SECTOR_ADDRESS_BITS == 64
   #define NARF_SECTOR_ADDRESS_TYPE uint64_t
#else
   #error "unrecognized NARF_SECTOR_ADDRESS_BITS value"
#endif

// Uncomment this for MBR and partitioning functions
// Useful for real removable media
#define NARF_MBR_UTILS

// Uncomment this for narf_defrag().
// Commenting it out will save code space.
#define NARF_USE_DEFRAG

#endif
