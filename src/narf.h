#ifndef _INCLUDE_NARF_FS_H_
#define _INCLUDE_NARF_FS_H_

#include <stdint.h>
#include <stdbool.h>

#include "narf_conf.h"

//! @brief Type for NARF sectors
//! @see narf_conf.h
//!
//! This is provided in the narf_conf.h file
typedef NARF_SECTOR_ADDRESS_TYPE NarfSector;

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
typedef NarfSector NAF;

//! @brief Type for sizes in bytes
typedef NARF_SIZE_TYPE NarfByteSize;

//! @brief An invalid NAF
//!
//! This usually indicates an error.
#define INVALID_NAF ((NAF) -1)

#ifdef NARF_MBR_UTILS
//! @brief Write a new blank MBR to the media
//! @see narf_partition
//! @see narf_format
//! @see narf_findpart
//! @see narf_mount
//!
//! VERY DESTRUCTIVE !!!
//!
//! existing MBR is overwritten and blanked
//!
//! @param message Custom boot_code message, or NULL for default
//! @return true for success
bool narf_mbr(const char *message);

//! @brief Write a new partition table entry to the media
//! @see narf_mbr
//! @see narf_format
//! @see narf_findpart
//! @see narf_mount
//!
//! DESTRUCTIVE !!!
//!
//! existing partition data is overwritten.
//! all available space is used for the new partition.
//!
//! @param partition The partition number (1-4) to occupy
//! @return true for success
bool narf_partition(int partition);

//! @brief Format a partition with a new NARF
//! @see narf_mbr
//! @see narf_partition
//! @see narf_findpart
//! @see narf_mount
//! @see narf_mkfs
//!
//! DESTRUCTIVE !!!
//!
//! calls narf_mkfs() with correct parameters based on partition
//! table.
bool narf_format(int partition);

//! @brief Find a NARF partition
//! @see narf_mbr
//! @see narf_partition
//! @see narf_format
//! @see narf_findpart
//! @see narf_mount
//!
//! @return A number (1-4) of the partition containint NARF, or -1
int narf_findpart(void);

//! @brief Mount a NARF partition
//! @see narf_mbr
//! @see narf_partition
//! @see narf_format
//! @see narf_findpart
//! @see narf_mount
//! @see narf_init
//!
//! calls narf_init with correct parameters based on partition
//! table.
//!
//! @param partition The partition (1-4) to mount
//! @return true for success
bool narf_mount(int partition);
#endif

//! @brief Create a new NARF
//!
//! This is for nonremovable media like flash memory.
//!
//! If you're working with removable media with MBR,
//! use narf_format() instead.  narf_format() will
//! call this with correct parameters for you.
//!
//! This should be used on blank media to initialize
//! the base data structure.  This is a destructive
//! operation, and wil overwrite any data already
//! present.
//!
//! One of either narf_mkfs() or narf_init() must
//! be called before other functions can be used
//!
//! @param start The first sector
//! @paran size The number of sectors
//! @return true for success
bool narf_mkfs(NarfSector start, NarfSector size);

//! @brief Initialize an existing NARF
//!
//! This is for nonremovable media like flash memory.
//!
//! If you're working with removable media with MBR,
//! use narf_mount() instead.  narf_mount() will
//! call this with correct parameters for you.
//!
//! This should be used on media that already has
//! a NARF, usually created by narf_mkfs()
//!
//! One of either narf_mkfs() or narf_init() must
//! be called before other functions can be used
//!
//! @param start The first sector
//! @return true for success
bool narf_init(NarfSector start);

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
//! @see narf_realloc()
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
//! @param bytes The size in bytes to reserve for data
//! @return The new NAF
NAF narf_alloc(const char *key,
               NarfByteSize    bytes);

//! @brief Grow or shrink storage for key
//! @see narf_alloc()
//! @see narf_free()
//! @see narf_rebalance()
//!
//! Grow or shrink the key allocation.  Like it's
//! C stdlib namesake, it may move the NAF. If
//! a NAF for the key does not exist, narf_alloc()
//! is called to create one.  If bytes == 0,
//! narf_free() is called and INVALID_NAF is returned.
//!
//! @param key The key we're reallocating
//! @param bytes The new size in bytes to reserve for data
//! @return The new NAF
NAF narf_realloc(const char *key,
                 NarfByteSize    bytes);

//! @brief Free storage for key
//! @see narf_alloc()
//! @see narf_realloc()
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
//! may call this for you under some circumstances.
//!
//! @return true for success
bool narf_rebalance(void);

//! @brief Defragment and compact the NARF
//! @see narf_free()
//!
//! This makes the NARF as small as possible by
//! removing any gaps that may have been left by
//! narf_free()
bool narf_defrag(void);

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
NarfSector narf_sector(NAF naf);

//! @brief Get the data size in bytes for this NAF
//!
//! @param naf The NAF
//! @return the size in bytes
NarfByteSize narf_size(NAF naf);

//! @brief Get the first NAF in key order
//! @see narf_next()
//! @see narf_last()
//! @see narf_previous()
//!
//! This is useful if you need to traverse all
//! NAFs in key order.
//!
//! @return the first NAF in key order
NAF narf_first(void);

//! @brief Get the next NAF in key order
//! @see narf_first()
//! @see narf_last()
//! @see narf_previous()
//!
//! This is useful if you need to traverse all
//! NAFs in key order.
//!
//! @param naf the current NAF
//! @return the NAF after the current, in key order
NAF narf_next(NAF naf);

//! @brief Get the last NAF in key order
//! @see narf_previous()
//! @see narf_first()
//! @see narf_next()
//!
//! This is useful if you need to traverse all
//! NAFs in reverse key order.
//!
//! @return the last NAF in key order
NAF narf_last(void);

//! @brief Get the previous NAF in key order
//! @see narf_last()
//! @see narf_last()
//! @see narf_previous()
//!
//! This is useful if you need to traverse all
//! NAFs in reverse key order.
//!
//! @param naf the current NAF
//! @return the NAF before the current, in key order
NAF narf_previous(NAF naf);

//! @brief Get metadata associated with NAF
//! @see narf_set_metadata()
//!
//! Returns a pointer to memory which WILL be
//! overwritten by any subsequent narf_*() call!
//!
//! Retrieve the metadata associated with the NAF.
//! This is an array of 32 bytes.  NARF does not
//! do anything with this data, you may assign any
//! use to it you like.
//!
//! metadata is preserved when narf_realloc() is
//! called with a new NONZERO size, even if new
//! storage is allocated.
//!
//! metadata is destroyed on narf_free() or if
//! narf_realloc() is called with a zero size.
//!
//! @param naf The NAF to get the metadata from
//! @return A pointer to an array of 32 bytes
void *narf_metadata(NAF naf);

//! @brief Set metadata associated with NAF
//! @see narf_metadata()
//!
//! see narf_metadata() for details
//!
//! @param naf The NAF to set metadata for
//! @param data Pointer to array of 32 bytes
//! @return true on success, false on failure
bool narf_set_metadata(NAF naf, void *data);

//! @brief Append data to a NAF
//!
//! @param key The key holding data to append to
//! @param data Pointer to the data
//! @param size The size of the data in bytes
//! @return true for success
bool narf_append(const char *key, const void *data, NarfByteSize size);

#ifdef NARF_DEBUG
//! @brief Print some debug info
void narf_debug(NAF naf);
#endif

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
