#ifndef _INCLUDE_NARF_FS_H_
#define _INCLUDE_NARF_FS_H_

#include <stdint.h>
#include <stdbool.h>

#include "narf_conf.h"

typedef NARF_SECTOR_ADDRESS_TYPE NarfSector;
typedef NARF_SIZE_TYPE NarfByteSize;

#define INVALID_NAF ((NarfSector) -1)

#ifdef NARF_MBR_UTILS
//! @brief Write a basic MBR sector.
//!
//! @param message Optional boot-code-area message, or NULL for the default.
//! @return true on success.
bool narf_mbr(const char *message);

//! @brief Create or replace a NARF partition table entry.
//!
//! @param partition Partition number, 1 through 4.
//! @return true on success.
bool narf_partition(int partition);

//! @brief Format an existing NARF partition.
//!
//! @param partition Partition number, 1 through 4.
//! @return true on success.
bool narf_format(int partition);

//! @brief Find the first NARF partition in the MBR.
//!
//! @return Partition number on success, or -1 when none is found.
int narf_findpart(void);

//! @brief Mount a NARF partition.
//!
//! @param partition Partition number, 1 through 4.
//! @return true on success.
bool narf_mount(int partition);
#endif

//! @brief Format a NARF filesystem at a sector origin.
//!
//! @param start Origin sector of the filesystem.
//! @param size Size of the filesystem in sectors.
//! @return true on success.
bool narf_mkfs(NarfSector start, NarfSector size);

//! @brief Mount a NARF filesystem at a sector origin.
//!
//! @param start Origin sector of the filesystem.
//! @return true on success.
bool narf_init(NarfSector start);

//! @brief Check whether a key exists.
//!
//! @param key NUL-terminated key string.
//! @return true if the key exists.
bool narf_find(const char *key);

//! @brief Return the first key directly under a directory prefix.
//!
//! @param dirname Directory prefix to scan.
//! @param sep Separator string, usually "/".
//! @return Pointer to an internal key buffer, or NULL when no entry exists.
const char *narf_dirfirst(const char *dirname, const char *sep);

//! @brief Return the next key directly under a directory prefix.
//!
//! @param dirname Directory prefix to scan.
//! @param sep Separator string, usually "/".
//! @param previous_key Key returned by a previous directory scan call.
//! @return Pointer to an internal key buffer, or NULL when no later entry exists.
const char *narf_dirnext(const char *dirname, const char *sep, const char *previous_key);

//! @brief Create a key with zero-filled payload storage.
//!
//! @param key NUL-terminated key string.
//! @param bytes Initial byte size.
//! @return true on success.
bool narf_alloc(const char *key, NarfByteSize bytes);

//! @brief Resize an existing key.
//!
//! @param key NUL-terminated key string.
//! @param bytes New byte size.
//! @return true on success.
bool narf_realloc(const char *key, NarfByteSize bytes);

//! @brief Compatibility wrapper around narf_realloc().
bool narf_realloc_key(const char *key, NarfByteSize bytes);

//! @brief Rename an existing key.
//!
//! @param key Existing key.
//! @param newkey New key.
//! @return true on success.
bool narf_rename_key(const char *key, const char *newkey);

//! @brief Delete a key.
//!
//! @param key Existing key.
//! @return true on success.
bool narf_free(const char *key);

//! @brief Compatibility wrapper around narf_free().
bool narf_free_key(const char *key);

#ifdef NARF_USE_DEFRAG
//! @brief Defragment the filesystem when supported.
//!
//! @return true on success.
bool narf_defrag(void);
#endif

//! @brief Return the physical sector for a key payload.
//!
//! @param key Existing key.
//! @return Physical sector, or INVALID_NAF when the key has no payload sector.
NarfSector narf_sector(const char *key);

//! @brief Return the payload byte size for a key.
//!
//! @param key Existing key.
//! @return Byte size, or 0 on failure.
NarfByteSize narf_size(const char *key);

//! @brief Return a copy of the key metadata area.
//!
//! @param key Existing key.
//! @return Pointer to an internal metadata buffer, or NULL on failure.
void *narf_metadata(const char *key);

//! @brief Replace the key metadata area.
//!
//! @param key Existing key.
//! @param data Pointer to metadata bytes.
//! @return true on success.
bool narf_set_metadata(const char *key, void *data);

//! @brief Atomically write bytes at an offset in a key payload.
//!
//! @param key Existing key.
//! @param data Bytes to write, or NULL to write zeroes.
//! @param size Number of bytes to write.
//! @param offset Byte offset in the payload.
//! @return true on success.
bool narf_write(const char *key, const void *data, NarfByteSize size, NarfByteSize offset);

//! @brief Append bytes to a key payload.
//!
//! @param key Existing key.
//! @param data Bytes to append.
//! @param size Number of bytes to append.
//! @return true on success.
bool narf_append(const char *key, const void *data, NarfByteSize size);

//! @brief Compatibility wrapper around narf_append().
bool narf_append_key(const char *key, const void *data, NarfByteSize size);

#ifdef NARF_DEBUG
//! @brief Print internal NARF root and tree state.
void narf_debug(void);
#endif

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
