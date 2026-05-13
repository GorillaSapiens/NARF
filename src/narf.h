#ifndef _INCLUDE_NARF_FS_H_
#define _INCLUDE_NARF_FS_H_

#include <stdint.h>
#include <stdbool.h>

#include "narf_conf.h"

typedef NARF_SECTOR_ADDRESS_TYPE NarfSector;
typedef NARF_SIZE_TYPE NarfByteSize;

#define INVALID_NAF ((NarfSector) -1)

#ifdef NARF_MBR_UTILS
bool narf_mbr(const char *message);
bool narf_partition(int partition);
bool narf_format(int partition);
int narf_findpart(void);
bool narf_mount(int partition);
#endif

bool narf_mkfs(NarfSector start, NarfSector size);
bool narf_init(NarfSector start);

bool narf_find(const char *key);

const char *narf_dirfirst(const char *dirname, const char *sep);
const char *narf_dirnext(const char *dirname, const char *sep, const char *previous_key);

bool narf_alloc(const char *key, NarfByteSize bytes);
bool narf_realloc(const char *key, NarfByteSize bytes);
bool narf_realloc_key(const char *key, NarfByteSize bytes);
bool narf_rename_key(const char *key, const char *newkey);
bool narf_free(const char *key);
bool narf_free_key(const char *key);

#ifdef NARF_USE_DEFRAG
bool narf_defrag(void);
#endif

NarfSector narf_sector(const char *key);
NarfByteSize narf_size(const char *key);
void *narf_metadata(const char *key);
bool narf_set_metadata(const char *key, void *data);
bool narf_append(const char *key, const void *data, NarfByteSize size);
bool narf_append_key(const char *key, const void *data, NarfByteSize size);

#ifdef NARF_DEBUG
void narf_debug(void);
#endif

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
