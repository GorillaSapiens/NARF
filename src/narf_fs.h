#ifndef _INCLUDE_NARF_FS_H_
#define _INCLUDE_NARF_FS_H_

#define NARF_DEBUG

//! @brief Create a NARF
//!
//! This should be used on blank media to set
//! the base data structure.  This is a destructive
//! operation, and wil overwrite any data already
//! present.
//!
//! @param sectors The total size in sectors
//! @return true for success
bool narf_mkfs(uint32_t sectors);

//! @brief Initialize a NARF
//!
//! This should be used on media that already has
//! a NARF, usually created by narf_mkfs()
//!
//! @return true for success
bool narf_init(void);

//! @brief Sync the NARF to disk
//!
//! Flushes any in memory informaion out to the device.
//!
//! @return true for success
bool narf_sync(void);

//! @brief Find the sector number matching the key
//!
//! Given a key, find the sector corresponding to the key.
//!
//! @param key The key to look for
//! @return The sector of the key, or -1 if not found
uint32_t narf_find(const char *key);

//! @Get the first sector in directory
//!
//! Returns the sector of the first key in order sequence
//! whose key starts with "dirname" and does not contain "sep"
//! in the remainder of the key, except at the end.
//!
//! This allows you to treat keys as if they were full
//! paths in a real file system.
//!
//! For rational use, dirname should end with sep.
//!
//! @param dirname Directory name with trailing seperator
//! @param sep Directory seperator
//! @return The sector of the key, or -1 if not found
uint32_t narf_dirfirst(const char *dirname, const char *sep);

//! @brief Get the next sector in directory
//!
//! Returns the sector of the next key in order sequence
//! whose key starts with "dirname" and does not contain "sep"
//! in the remainder the key, except at the end.
//!
//! This allows you to treat keys as if they were full
//! paths in a real file system.
//!
//! For rational use, dirname should end with sep.
//!
//! @param dirname Directory name with trailing seperator
//! @param sep Directory seperator
//! @param the previous sector
//! @return The sector of the key, or -1 if not found
uint32_t narf_dirnext(const char *dirname, const char *sep, uint32_t sector);

//! @brief Allocate storage for key
//!
//! Create and allocate storage for a key.  If the insertion
//! depth exceeds 48, a rebalance is performed (this is 1.5
//! times the ideal height of a tree with 2^32 nodes)
//!
//! @param key The key we're allocating for
//! @param size The size in bytes to reserve for the nes data
//! @return The new sector
uint32_t narf_alloc(const char *key, uint32_t size);

//! @brief Free storage for key
//!
//! Frees up (deletes) space allocated by narf_alloc(),
//! including the key itself.
//!
//! @param key The key we're freeing
//! @return true for success
bool narf_free(const char *key);

//! @brief Rebalance the entire tree
//!
//! Use this after a large number of writes cause
//! the NARF to become unbalanced.  This
//! is called automatically by narf_free(), as
//! freeing a node is a complex process.
//!
//! @return true for success
bool narf_rebalance(void);

//! @brief Get the key corresponding to a sector
//!
//! @return pointer to static buffer overwritten each call!
const char *narf_get_key(uint32_t sector);

//! @brief Get the data sector reserved for this sector
//!
//! Sector is the value returned by narf_alloc(), narf_find(), etc...
//!
//! @param sector The NARF sector
//! @return a sector number or -1
uint32_t narf_get_data_sector(uint32_t sector);

//! @brief Get the data size in bytes reserved for this sector
//!
//! Sector is the value returned by narf_alloc(), narf_find(), etc...
//!
//! @param sector The NARF sector
//! @return the size in bytes
uint32_t narf_get_data_size(uint32_t sector);

//! @brief Get the next sector in order after the given sector
//!
//! @param sector The NARF sector
//! @return a NARF sector continaing the next key
uint32_t narf_get_next(uint32_t sector);

#ifdef NARF_DEBUG
//! @brief Print some debug info
void narf_debug(void);
#endif

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
