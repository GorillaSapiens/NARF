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

// Uncomment this for "smart" free implementation
// This is faster, but takes up more code space
#define NARF_SMART_FREE

// Uncomment this for narf_rebalance() to use malloc memory
// for temporary key storage.  Leave it commented to use
// function static storage.  It is generally too large to
// store on the stack on small systems.
#define NARF_MALLOC

// Uncomment this for utf-8 code point based key comparison
//
// you probably don't need this.  i can't think of any reason
// anyone would need this. UTF-8 ensures that higher code
// points have higher byte values.  multibyte characters are
// encoded in a way that preserves ordering.  this define has
// slightly different handling in the edge case where a UTF-8
// character is malformed, which could happen in the case of
// the complete key not fitting in the alloted space.
//
// i'll leave it here, commented, to serve as a model in case
// someone wants to do some different kind of key ordering.
//
//#define UTF8_STRNCMP

#endif
