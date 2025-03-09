#ifndef _INCLUDE_NARF_FS_H_
#define _INCLUDE_NARF_FS_H_

/// Format (blank) a file system.  destructive !!!
///
/// @return true on success
bool narf_format(void);

/// Create a new directory
///
/// @param sector The parent directory
/// @param name The name of the directory
/// @return true if successful, false if sector is not a directory
bool narf_mkdir(uint32_t sector, const char *name);

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
