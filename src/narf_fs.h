#ifndef _INCLUDE_NARF_FS_H_
#define _INCLUDE_NARF_FS_H_

#define NARF_DEBUG

/// Create a NARF
///
/// @param sectors The total size in sectors
/// @return true for success
bool narf_mkfs(uint32_t sectors);

/// Initialize a NARF
///
/// @return true for success
bool narf_init(void);

/// sync the NARF to disk
///
/// @return true for success
bool narf_sync(void);

/// Find the sector number matching the key
///
/// @param key The key to look for
/// @return The sector of the key, or -1 if not found
uint32_t narf_find(const char *key);

/// Find the sector number matching the key substring
///
/// @param key The key to look for
/// @return The sector of the key, or -1 if not found
uint32_t narf_dirfind(const char *key);

/// Allocate storage for key
///
/// @param key The key we're allocating for
/// @return The new sector
uint32_t narf_alloc(const char *key, uint32_t size);

/// Free storage for key
///
/// @param key The key we're freeing
/// @return true for success
bool narf_free(const char *key);

/// Rebalance the entire tree
///
/// @return true for success
bool narf_rebalance(void);

/// Get the name
///
/// returns pointer to static buffer overwritten each call!
///
const char *narf_get_name(uint32_t sector);

/// Get the data sector
///
uint32_t narf_get_data_sector(uint32_t sector);

/// Get the data size in bytes
///
uint32_t narf_get_data_size(uint32_t sector);

/// Get the data sector
///
uint32_t narf_get_next(uint32_t sector);

#ifdef NARF_DEBUG
/// Print some debug info
void narf_debug(void);
#endif

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
