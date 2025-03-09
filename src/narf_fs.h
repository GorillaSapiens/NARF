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
/// @return The sector of the key, or NARF_TAIL if not found
uint32_t narf_find(const char *key);

/// Find the sector number matching the key substring
///
/// @param key The key to look for
/// @return The sector of the key, or NARF_TAIL if not found
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

/// insert sector into the tree
///
/// @return true for success
bool narf_insert(uint32_t sector, const uint8_t *key);

#ifdef NARF_DEBUG
/// Print some debug info
void narf_debug(void);
#endif

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
