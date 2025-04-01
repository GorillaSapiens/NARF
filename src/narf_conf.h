#ifndef _INCLUDE_NARF_CONF_H_
#define _INCLUDE_NARF_CONF_H_

// Tunable parameters go in this file

// Size of a sector in bytes
// beware of large values, NARF keeps a sector sized
// buffer in memory.
#define NARF_SECTOR_SIZE 512

// Type used to store sector addresses
// should be one of uint8_t, uint16_t, uint32_t, or uint64_t
#define NARF_SECTOR_ADDRESS_TYPE uint32_t

// Type used to store byte sizes of a NAF
// should be one of uint8_t, uint16_t, uint32_t, or uint64_t
#define NARF_SIZE_TYPE uint32_t

// Uncomment this for MBR and partitioning functions
// Useful for real removable media
#define NARF_MBR_UTILS

// Uncomment this for debugging functions
#define NARF_DEBUG

// Uncomment this for debugging structure integrity
//
// Beware, this makes EVERYTHING very slow!
//
//#define NARF_DEBUG_INTEGRITY

// Uncomment for unicode line drawing characters in debug functions
#define USE_UTF8_LINE_DRAWING

// Uncomment this for narf_rebalance() to use malloc memory
// for temporary key storage.  Leave it commented to use
// function static storage.  It is generally too large to
// store on the stack on small systems.
#define NARF_MALLOC

#endif
