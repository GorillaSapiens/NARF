#ifndef _INCLUDE_NARF_FS_H_
#define _INCLUDE_NARF_FS_H_

#include <stdint.h>
#include <stdbool.h>

// define this for debug functions
//#define NARF_DEBUG

//! @brief Type for NARF entries
//!
//! A NAF is Not A File.  It is an entry containing
//! a key, and the location of the data associated
//! with the key.
//!
//! Under the hood this is just a sector number, but
//! we want to make clear the distinction between a
//! NAF, which contains metadata, and a regular sector
//! containing data.
typedef uint32_t NAF;

//! @brief An invalid NAF
//!
//! This usually indicates an error.
#define INVALID_NAF ((NAF) -1)

//! @brief Create a new NARF
//!
//! This should be used on blank media to initialize
//! the base data structure.  This is a destructive
//! operation, and wil overwrite any data already
//! present.
//!
//! One of either narf_mkfs() or narf_init() must
//! be called before other functions can be used
//!
//! @param sectors The total size in sectors
//! @return true for success
bool narf_mkfs(uint32_t sectors);

//! @brief Initialize an existing NARF
//!
//! This should be used on media that already has
//! a NARF, usually created by narf_mkfs()
//!
//! One of either narf_mkfs() or narf_init() must
//! be called before other functions can be used
//!
//! @return true for success
bool narf_init(void);

//! @brief Sync the NARF to disk
//!
//! Flushes any in memory informaion out to the device.
//!
//! This MUST be called before narf_io_close() is
//! called.
//!
//! @return true for success
bool narf_sync(void);

//! @brief Find the NAF matching the key
//! @see narf_key()
//!
//! Given a key, find the NAF corresponding to the key.
//!
//! @param key The key to look for
//! @return The NAF of the key, or INVAID_NAF if not found
NAF narf_find(const char *key);

//! @brief Get the first NAF in directory
//! @see narf_dirnext()
//!
//! Returns the NAF of the first key in order
//! sequence whose key starts with "dirname" and
//! does not contain "sep" in the remainder of the
//! key, except at the end.
//!
//! This allows you to treat keys as if they were full
//! paths in a real file system.
//!
//! For rational use, dirname should end with sep.
//!
//! @param dirname Directory name with trailing separator
//! @param sep Directory separator
//! @return The NAF, or INVALID_NAF if not found
NAF narf_dirfirst(const char *dirname,
                  const char *sep);

//! @brief Get the next NAF in directory
//! @see narf_dirfirst()
//!
//! Returns the NAF of the next key in order
//! sequence whose key starts with "dirname" and
//! does not contain "sep" in the remainder of the
//! key, except at the end.
//!
//! This allows you to treat keys as if they were full
//! paths in a real file system.
//!
//! For rational use, dirname should end with sep.
//!
//! @param dirname Directory name with trailing separator
//! @param sep Directory separator
//! @param naf The previous NAF
//! @return The next NAF, or INVALID_NAF if not found
NAF narf_dirnext(const char *dirname,
                 const char *sep,
                 NAF         naf);

//! @brief Allocate storage for key
//! @see narf_free()
//! @see narf_rebalance()
//!
//! Create and allocate storage for a key.
//!
//! If the insertion depth of the internal binary
//! tree exceeds 48, a rebalance is performed.
//! (48 is 1.5 times the ideal height of a tree
//! with 2^32 nodes)
//!
//! @param key The key we're allocating for
//! @param size The size in bytes to reserve for data
//! @return The new NAF
NAF narf_alloc(const char *key,
               uint32_t    size);

//! @brief Free storage for key
//! @see narf_alloc()
//! @see narf_rebalance()
//!
//! Frees up (deletes) space allocated by narf_alloc(),
//! including the key itself.
//!
//! This will always call narf_rebalance()
//!
//! @param key The key we're freeing
//! @return true for success
bool narf_free(const char *key);

//! @brief Rebalance the entire tree
//! @see narf_alloc()
//! @see narf_free()
//!
//! Rebalances the NARF binary tree.  This is an
//! expensive operation.
//!
//! Other NARF functions (narf_alloc(), narf_free())
//! may call this for you.
//!
//! @return true for success
bool narf_rebalance(void);

//! @brief Get the key corresponding to a NAF
//! @see narf_find()
//!
//! Given a NAF, return the key for the NAF.
//!
//! This returns a pointer to a static buffer which
//! may be overwritten by other NARF functions.
//!
//! This is the inverse function of narf_find().
//!
//! @return the key for the NAF
const char *narf_key(NAF naf);

//! @brief Get the first data sector reserved for this NAF
//!
//! @param naf The NAF
//! @return a sector number or -1
uint32_t narf_sector(NAF naf);

//! @brief Get the data size in bytes for this NAF
//!
//! @param naf The NAF
//! @return the size in bytes
uint32_t narf_size(NAF naf);

//! @brief Get the first NAF in key order
//! @see narf_next()
//!
//! This is useful if you need to traverse all
//! NAFs in key order.
//!
//! @return the first NAF in key order
NAF narf_first(void);

//! @brief Get the next NAF in key order
//! @see narf_first()
//!
//! This is useful if you need to traverse all
//! NAFs in key order.
//!
//! @param naf the current NAF
//! @return the NAF after the current, in key order
NAF narf_next(NAF naf);

#ifdef NARF_DEBUG
//! @brief Print some debug info
void narf_debug(void);
#endif

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
