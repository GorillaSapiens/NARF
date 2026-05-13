#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <inttypes.h>
#include <time.h>
#include <sys/xattr.h>
#include <grp.h>

#include "narf_conf.h"
#include "narf_io.h"
#include "narf.h"

// NARF is not thread safe.  so here we have a mutex, used
// by all FUSE functions, to guarantee single threaded access.
static pthread_mutex_t narf_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK   pthread_mutex_lock(&narf_mutex)
#define UNLOCK pthread_mutex_unlock(&narf_mutex)

static int fd;
static off_t size;
static int partition = -1;
static bool mounted = false;
static time_t mount_time;

char *xformpath(const char *path);

#define NARF_XATTR_PREFIX "user."
#define NARF_META_VERSION "v1"

// A compact, human-readable metadata string lives in each NARF node:
//
//    v1 uid=1000 gid=1000 mode=100644 mtime=1778706214 bs=4K
//
// FUSE parses the Unix-ish fields and exposes every other key=value token as
// a user.<key> extended attribute.  Unknown tokens are preserved when known
// fields are changed.  Tokens with whitespace cannot be represented here.
typedef struct {
   uid_t uid;
   gid_t gid;
   mode_t mode;
   time_t mtime;
} NarfFuseMeta;

static time_t now_sec(void) {
   time_t t = time(NULL);

   if (t == (time_t) -1) {
      return 0;
   }

   return t;
}

static time_t default_mtime(void) {
   if (mount_time != 0) {
      return mount_time;
   }

   return now_sec();
}

static void meta_defaults(NarfFuseMeta *meta, mode_t mode) {
   meta->uid = getuid();
   meta->gid = getgid();
   meta->mode = mode;
   meta->mtime = default_mtime();
}

static bool reserved_meta_key(const char *key) {
   return !strcmp(key, "uid") || !strcmp(key, "gid") ||
          !strcmp(key, "mode") || !strcmp(key, "mtime");
}

static bool custom_meta_key_ok(const char *key) {
   const unsigned char *p = (const unsigned char *) key;

   if (key == NULL || *key == 0) {
      return false;
   }

   while (*p) {
      if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.')) {
         return false;
      }
      p++;
   }

   return true;
}

static bool custom_meta_value_ok(const char *value) {
   const unsigned char *p = (const unsigned char *) value;

   if (value == NULL) {
      return false;
   }

   while (*p) {
      if (*p <= ' ' || *p == 0x7f) {
         return false;
      }
      p++;
   }

   return true;
}

static bool parse_uintmax(const char *s, uintmax_t *out, int base) {
   char *end = NULL;
   uintmax_t v;

   if (s == NULL || *s == 0 || out == NULL) {
      return false;
   }

   errno = 0;
   v = strtoumax(s, &end, base);

   if (errno || end == s || *end != 0) {
      return false;
   }

   *out = v;
   return true;
}

static void parse_metadata_string(const char *metadata, NarfFuseMeta *meta) {
   char tmp[NARF_METADATA_SIZE];
   char *save = NULL;
   char *token;

   if (metadata == NULL || meta == NULL) {
      return;
   }

   snprintf(tmp, sizeof(tmp), "%s", metadata);
   token = strtok_r(tmp, " ", &save);

   if (token == NULL || strcmp(token, NARF_META_VERSION)) {
      return;
   }

   while ((token = strtok_r(NULL, " ", &save)) != NULL) {
      char *eq = strchr(token, '=');
      char *key;
      char *value;
      uintmax_t v;

      if (eq == NULL) {
         continue;
      }

      *eq = 0;
      key = token;
      value = eq + 1;

      if (!strcmp(key, "uid")) {
         if (parse_uintmax(value, &v, 10) && (uid_t) v == v) {
            meta->uid = (uid_t) v;
         }
      }
      else if (!strcmp(key, "gid")) {
         if (parse_uintmax(value, &v, 10) && (gid_t) v == v) {
            meta->gid = (gid_t) v;
         }
      }
      else if (!strcmp(key, "mode")) {
         if (parse_uintmax(value, &v, 8) && (mode_t) v == v) {
            meta->mode = (mode_t) v;
         }
      }
      else if (!strcmp(key, "mtime")) {
         if (parse_uintmax(value, &v, 10)) {
            meta->mtime = (time_t) v;
         }
      }
   }
}

static int read_metadata(const char *key, mode_t default_mode,
      NarfFuseMeta *meta, char metadata[NARF_METADATA_SIZE]) {
   void *raw;

   if (key == NULL || meta == NULL || metadata == NULL) {
      return -EINVAL;
   }

   raw = narf_metadata(key);
   if (raw == NULL) {
      return -ENOENT;
   }

   memcpy(metadata, raw, NARF_METADATA_SIZE);
   metadata[NARF_METADATA_SIZE - 1] = 0;

   meta_defaults(meta, default_mode);
   parse_metadata_string(metadata, meta);
   return 0;
}

static int append_token(char out[NARF_METADATA_SIZE], const char *key, const char *value) {
   size_t used = strlen(out);
   int n;

   if (used >= NARF_METADATA_SIZE) {
      return -ENOSPC;
   }

   n = snprintf(out + used, NARF_METADATA_SIZE - used, " %s=%s", key, value);
   if (n < 0 || (size_t) n >= NARF_METADATA_SIZE - used) {
      return -ENOSPC;
   }

   return 0;
}

static int build_metadata(const char *old_metadata, const NarfFuseMeta *meta,
      const char *set_key, const char *set_value, bool remove_key,
      char out[NARF_METADATA_SIZE]) {
   char tmp[NARF_METADATA_SIZE];
   char *save = NULL;
   char *token;
   int n;

   if (meta == NULL || out == NULL) {
      return -EINVAL;
   }

   memset(out, 0, NARF_METADATA_SIZE);
   n = snprintf(out, NARF_METADATA_SIZE,
         "%s uid=%ju gid=%ju mode=%06jo mtime=%jd",
         NARF_META_VERSION,
         (uintmax_t) meta->uid,
         (uintmax_t) meta->gid,
         (uintmax_t) meta->mode,
         (intmax_t) meta->mtime);

   if (n < 0 || (size_t) n >= NARF_METADATA_SIZE) {
      return -ENOSPC;
   }

   if (old_metadata != NULL) {
      snprintf(tmp, sizeof(tmp), "%s", old_metadata);
      token = strtok_r(tmp, " ", &save);

      if (token != NULL && !strcmp(token, NARF_META_VERSION)) {
         while ((token = strtok_r(NULL, " ", &save)) != NULL) {
            char *eq = strchr(token, '=');
            char *key;
            char *value;
            int ret;

            if (eq == NULL) {
               continue;
            }

            *eq = 0;
            key = token;
            value = eq + 1;

            if (reserved_meta_key(key) || !custom_meta_key_ok(key) ||
                  !custom_meta_value_ok(value)) {
               continue;
            }

            if (set_key != NULL && !strcmp(key, set_key)) {
               continue;
            }

            if (remove_key && set_key != NULL && !strcmp(key, set_key)) {
               continue;
            }

            ret = append_token(out, key, value);
            if (ret != 0) {
               return ret;
            }
         }
      }
   }

   if (set_key != NULL && !remove_key) {
      return append_token(out, set_key, set_value != NULL ? set_value : "");
   }

   return 0;
}

static int write_metadata_string(const char *key, const char metadata[NARF_METADATA_SIZE]) {
   uint8_t raw[NARF_METADATA_SIZE];

   memset(raw, 0, sizeof(raw));
   snprintf((char *) raw, sizeof(raw), "%s", metadata);

   if (!narf_set_metadata(key, raw)) {
      return -EIO;
   }

   return 0;
}

static int prepare_metadata_update(const char *key, mode_t default_mode,
      void (*change)(NarfFuseMeta *meta, void *arg), void *arg,
      char out[NARF_METADATA_SIZE]) {
   char old_metadata[NARF_METADATA_SIZE];
   NarfFuseMeta meta;
   int ret;

   ret = read_metadata(key, default_mode, &meta, old_metadata);
   if (ret != 0) {
      return ret;
   }

   if (change != NULL) {
      change(&meta, arg);
   }

   return build_metadata(old_metadata, &meta, NULL, NULL, false, out);
}

static int commit_metadata_update(const char *key, mode_t default_mode,
      void (*change)(NarfFuseMeta *meta, void *arg), void *arg) {
   char metadata[NARF_METADATA_SIZE];
   int ret;

   ret = prepare_metadata_update(key, default_mode, change, arg, metadata);
   if (ret != 0) {
      return ret;
   }

   return write_metadata_string(key, metadata);
}

static void set_mtime_change(NarfFuseMeta *meta, void *arg) {
   const time_t *mtime = (const time_t *) arg;

   meta->mtime = *mtime;
}

static int init_metadata_for_key(const char *key, mode_t mode) {
   NarfFuseMeta meta;
   char metadata[NARF_METADATA_SIZE];
   int ret;

   meta_defaults(&meta, mode);
   meta.mtime = now_sec();

   ret = build_metadata(NULL, &meta, NULL, NULL, false, metadata);
   if (ret != 0) {
      return ret;
   }

   return write_metadata_string(key, metadata);
}

static int key_for_existing_path(const char *path, char key[NARF_SECTOR_SIZE],
      bool *is_dir) {
   int n;
   char *dirkey;

   if (path == NULL || key == NULL || is_dir == NULL) {
      return -EINVAL;
   }

   if (!strcmp(path, "/")) {
      return -EINVAL;
   }

   path++;
   n = snprintf(key, NARF_SECTOR_SIZE, "%s", path);
   if (n < 0 || (size_t) n >= NARF_SECTOR_SIZE) {
      return -ENAMETOOLONG;
   }

   if (narf_find(key)) {
      *is_dir = false;
      return 0;
   }

   dirkey = xformpath(path - 1);
   if (dirkey == NULL) {
      return -ENOMEM;
   }

   n = snprintf(key, NARF_SECTOR_SIZE, "%s", dirkey);
   free(dirkey);

   if (n < 0 || (size_t) n >= NARF_SECTOR_SIZE) {
      return -ENAMETOOLONG;
   }

   if (narf_find(key)) {
      *is_dir = true;
      return 0;
   }

   return -ENOENT;
}

static int metadata_for_path(const char *path, char key[NARF_SECTOR_SIZE],
      bool *is_dir, NarfFuseMeta *meta, char metadata[NARF_METADATA_SIZE]) {
   mode_t default_mode;
   int ret;

   ret = key_for_existing_path(path, key, is_dir);
   if (ret != 0) {
      return ret;
   }

   default_mode = *is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
   return read_metadata(key, default_mode, meta, metadata);
}

static int split_user_xattr(const char *name, const char **key) {
   size_t prefix_len = strlen(NARF_XATTR_PREFIX);

   if (name == NULL || strncmp(name, NARF_XATTR_PREFIX, prefix_len)) {
      return -ENOTSUP;
   }

   *key = name + prefix_len;

   if (!custom_meta_key_ok(*key) || reserved_meta_key(*key)) {
      return -ENOTSUP;
   }

   return 0;
}

static bool find_custom_value(const char *metadata, const char *wanted,
      char value[NARF_METADATA_SIZE]) {
   char tmp[NARF_METADATA_SIZE];
   char *save = NULL;
   char *token;

   if (metadata == NULL || wanted == NULL || value == NULL) {
      return false;
   }

   snprintf(tmp, sizeof(tmp), "%s", metadata);
   token = strtok_r(tmp, " ", &save);

   if (token == NULL || strcmp(token, NARF_META_VERSION)) {
      return false;
   }

   while ((token = strtok_r(NULL, " ", &save)) != NULL) {
      char *eq = strchr(token, '=');
      char *key;
      char *val;

      if (eq == NULL) {
         continue;
      }

      *eq = 0;
      key = token;
      val = eq + 1;

      if (!strcmp(key, wanted) && !reserved_meta_key(key) &&
            custom_meta_key_ok(key) && custom_meta_value_ok(val)) {
         snprintf(value, NARF_METADATA_SIZE, "%s", val);
         return true;
      }
   }

   return false;
}

static int set_custom_value(const char *key, bool is_dir, const char *xkey,
      const char *xvalue, bool remove_key) {
   char old_metadata[NARF_METADATA_SIZE];
   char metadata[NARF_METADATA_SIZE];
   NarfFuseMeta meta;
   mode_t default_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
   int ret;

   ret = read_metadata(key, default_mode, &meta, old_metadata);
   if (ret != 0) {
      return ret;
   }

   ret = build_metadata(old_metadata, &meta, xkey, xvalue, remove_key, metadata);
   if (ret != 0) {
      return ret;
   }

   return write_metadata_string(key, metadata);
}

static bool current_user_in_group(gid_t gid) {
   int count;
   gid_t *groups;
   bool found = false;

   if (gid == getgid()) {
      return true;
   }

   count = getgroups(0, NULL);
   if (count <= 0) {
      return false;
   }

   groups = malloc((size_t) count * sizeof(*groups));
   if (groups == NULL) {
      return false;
   }

   if (getgroups(count, groups) == count) {
      for (int i = 0; i < count; i++) {
         if (groups[i] == gid) {
            found = true;
            break;
         }
      }
   }

   free(groups);
   return found;
}

static int check_access_bits(const NarfFuseMeta *meta, int mask) {
   mode_t bits;

   if (mask == F_OK) {
      return 0;
   }

   if (getuid() == 0) {
      if ((mask & X_OK) && !(meta->mode & 0111)) {
         return -EACCES;
      }
      return 0;
   }

   if (getuid() == meta->uid) {
      bits = (meta->mode >> 6) & 7;
   }
   else if (current_user_in_group(meta->gid)) {
      bits = (meta->mode >> 3) & 7;
   }
   else {
      bits = meta->mode & 7;
   }

   if ((mask & R_OK) && !(bits & 4)) {
      return -EACCES;
   }

   if ((mask & W_OK) && !(bits & 2)) {
      return -EACCES;
   }

   if ((mask & X_OK) && !(bits & 1)) {
      return -EACCES;
   }

   return 0;
}

// strips leading slash if present,
// adds trailing slash if missing
// must be free'd !!!
char *xformpath(const char *path) {
   size_t n;
   char *p;

   if (path == NULL) return NULL;

   if (path[0] == '/') {
      path++;
   }

   n = strlen(path);
   p = malloc(n + 2);

   if (p == NULL) {
      return NULL;
   }

   memcpy(p, path, n);
   p[n] = 0;

   if (n == 0 || p[n - 1] != '/') {
      p[n] = '/';
      p[n + 1] = 0;
   }

   return p;
}

//! @brief Initialize the narf_io layer
//!
//! Used
//! This is typically implemented by you for yor
//! hardware.
//!
//! @return true on success
bool narf_io_open(void) {
   return true;
}

//! @brief Deinitialize the narf_io layer
//!
//! This is typically implemented by you for yor
//! hardware.
//!
//! @return true on success
bool narf_io_close(void) {
   return true;
}

//! @brief Get the size of the underlying hardware device in sectors
//!
//! This is typically implemented by you for yor
//! hardware.
//!
//! @return the number of sectors supported by the device
uint32_t narf_io_sectors(void) {
   return size / NARF_SECTOR_SIZE;
}

//! @brief Write a sector to the disk
//!
//! This is typically implemented by you for your
//! hardware.
//!
//! @param sector The address of the sector to access
//! @param data Pointer to 512 bytes of data to write
//! @return true on success
bool narf_io_write(uint32_t sector, void *data) {
   off_t offset;
   ssize_t written;

   if (data == NULL) {
      return false;
   }

   if (sector >= narf_io_sectors()) {
      return false;
   }

   offset = (off_t) sector * NARF_SECTOR_SIZE;

   if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
      return false;
   }

   written = write(fd, data, NARF_SECTOR_SIZE);

   if (written != NARF_SECTOR_SIZE) {
      return false;
   }

   fsync(fd);
   return true;
}

//! @brief Read a sector from the disk
//!
//! This is typically implemented by you for your
//! hardware.
//!
//! @param sector The address of the sector to access
//! @param data Pointer to 512 bytes read buffer
//! @return true on success
bool narf_io_read(uint32_t sector, void *data) {
   off_t offset;
   ssize_t bytes;

   if (data == NULL) {
      return false;
   }

   if (sector >= narf_io_sectors()) {
      return false;
   }

   offset = (off_t) sector * NARF_SECTOR_SIZE;

   if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
      return false;
   }

   bytes = read(fd, data, NARF_SECTOR_SIZE);

   if (bytes != NARF_SECTOR_SIZE) {
      return false;
   }

   return true;
}


// --- File & directory metadata ---
//! @brief FUSE getattr callback.
static int my_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
   (void) fi;

   if (!mounted) return -ENODEV;

   LOCK;

   memset(st, 0, sizeof(*st));

   // root always exists, but it does not have an on-disk NARF node.
   if (strcmp(path, "/") == 0) {
      st->st_mode = S_IFDIR | 0755;
      st->st_nlink = 2;
      st->st_uid = getuid();
      st->st_gid = getgid();
      st->st_size = 0;
      st->st_atime = default_mtime();
      st->st_ctime = default_mtime();
      st->st_mtime = default_mtime();
      UNLOCK;
      return 0;
   }

   // see if it exists as a file
   if (narf_find(path + 1)) {
      char metadata[NARF_METADATA_SIZE];
      NarfFuseMeta meta;

      if (read_metadata(path + 1, S_IFREG | 0644, &meta, metadata) != 0) {
         meta_defaults(&meta, S_IFREG | 0644);
      }

      st->st_mode = S_IFREG | (meta.mode & 07777);
      st->st_nlink = 1;
      st->st_uid = meta.uid;
      st->st_gid = meta.gid;
      st->st_size = narf_size(path + 1);
      st->st_atime = meta.mtime;
      st->st_ctime = meta.mtime;
      st->st_mtime = meta.mtime;
      st->st_blocks = (st->st_size + 511) / 512;
      UNLOCK;
      return 0;
   }

   // see if it exists as a directory
   char *p = xformpath(path);
   if (p == NULL) {
      UNLOCK;
      return -ENOMEM;
   }

   if (narf_find(p)) {
      char metadata[NARF_METADATA_SIZE];
      NarfFuseMeta meta;

      if (read_metadata(p, S_IFDIR | 0755, &meta, metadata) != 0) {
         meta_defaults(&meta, S_IFDIR | 0755);
      }

      st->st_mode = S_IFDIR | (meta.mode & 07777);
      st->st_nlink = 2;
      st->st_uid = meta.uid;
      st->st_gid = meta.gid;
      st->st_size = narf_size(p);
      st->st_atime = meta.mtime;
      st->st_ctime = meta.mtime;
      st->st_mtime = meta.mtime;
      free(p);
      UNLOCK;
      return 0;
   }

   // see if it exists as a phantom directory
   const char *dirent0 = narf_dirfirst(p, "/");
   free(p);
   if (dirent0 != NULL) {
      st->st_mode = S_IFDIR | 0755;
      st->st_nlink = 2;
      st->st_uid = getuid();
      st->st_gid = getgid();
      st->st_size = 0;
      st->st_atime = default_mtime();
      st->st_ctime = default_mtime();
      st->st_mtime = default_mtime();
      UNLOCK;
      return 0;
   }

   UNLOCK;
   return -ENOENT;
}

//! @brief FUSE access callback.
static int my_access(const char *path, int mask) {
   char key[NARF_SECTOR_SIZE];
   char metadata[NARF_METADATA_SIZE];
   NarfFuseMeta meta;
   bool is_dir;
   int ret;

   if (!mounted) return -ENODEV;

   if (!strcmp(path, "/")) {
      meta_defaults(&meta, S_IFDIR | 0755);
      return check_access_bits(&meta, mask);
   }

   LOCK;
   ret = metadata_for_path(path, key, &is_dir, &meta, metadata);

   if (ret == -ENOENT) {
      char *p = xformpath(path);

      if (p == NULL) {
         UNLOCK;
         return -ENOMEM;
      }

      if (narf_dirfirst(p, "/") != NULL) {
         meta_defaults(&meta, S_IFDIR | 0755);
         ret = 0;
      }

      free(p);
   }

   UNLOCK;

   if (ret != 0) {
      return ret;
   }

   (void) key;
   (void) is_dir;
   return check_access_bits(&meta, mask);
}

//! @brief FUSE readlink callback.
static int my_readlink(const char *path, char *buf, size_t size) {
   (void) path;
   (void) buf;
   (void) size;

   if (!mounted) return -ENODEV;

   // Return symlink target
   return -EINVAL;
}

//! @brief FUSE mknod callback.
static int my_mknod(const char *path, mode_t mode, dev_t rdev) {
   (void) path;
   (void) mode;
   (void) rdev;

   if (!mounted) return -ENODEV;

   // Create special files (FIFO, char/block dev, etc.)
   return -EPERM;
}

//! @brief FUSE mkdir callback.
static int my_mkdir(const char *path, mode_t mode) {
   int ret;

   if (!mounted) return -ENODEV;

   LOCK;

   // Create a directory
   if (narf_find(path + 1)) {
      // it already exists as a file
      UNLOCK;
      return -EEXIST;
   }

   char *p = xformpath(path);

   if (p == NULL) {
      UNLOCK;
      return -ENOMEM;
   }

   if (narf_find(p)) {
      // it already exists as a directory
      free(p);
      UNLOCK;
      return -EEXIST;
   }

   if (!narf_alloc(p, 0)) {
      free(p);
      UNLOCK;
      return -EIO;
   }

   ret = init_metadata_for_key(p, S_IFDIR | (mode & 07777));
   if (ret != 0) {
      narf_free_key(p);
      free(p);
      UNLOCK;
      return ret;
   }

   free(p);
   UNLOCK;
   return 0;
}

//! @brief FUSE unlink callback.
static int my_unlink(const char *path) {
   if (!mounted) return -ENODEV;

   LOCK;

   // Delete a file
   if (narf_free_key(path + 1)) {
      UNLOCK;
      return 0;
   }
   UNLOCK;
   return -EROFS;
}

//! @brief FUSE rmdir callback.
static int my_rmdir(const char *path) {
   if (!mounted) return -ENODEV;

   LOCK;

   // Remove a directory

   if (narf_find(path + 1)) {
      UNLOCK;
      return -ENOTDIR;
   }

   char *p = xformpath(path);
   const char *entry = narf_dirfirst(p, "/");

   if (entry == NULL) {
      free(p);
      UNLOCK;
      return -ENOENT;
   }

   if (strcmp(p, entry)) {
      free(p);
      UNLOCK;
      return -ENOTEMPTY;
   }

   entry = narf_dirnext(p, "/", entry);

   if (entry != NULL) {
      free(p);
      UNLOCK;
      return -ENOTEMPTY;
   }
   else {
      if (!narf_free_key(p)) {
         free(p);
         UNLOCK;
         return -EIO;
      }
      free(p);
   }

   UNLOCK;
   return 0;

   //return -EROFS;
}

//! @brief FUSE symlink callback.
static int my_symlink(const char *target, const char *linkpath) {
   (void) target;
   (void) linkpath;

   if (!mounted) return -ENODEV;

   // Create a symbolic link
   return -EROFS;
}

//! @brief FUSE rename callback.
static int my_rename(const char *oldpath, const char *newpath, unsigned int flags) {
   (void) flags;

   if (!mounted) return -ENODEV;

   // Rename or move file/directory

   LOCK;

   int ret = -EROFS;

   char *olddir = xformpath(oldpath);
   char *newdir = xformpath(newpath);

   if (olddir == NULL || newdir == NULL) {
      ret = -ENOMEM;
      goto fini;
   }

   bool oldfile = narf_find(oldpath + 1);
   bool newfile = narf_find(newpath + 1);
   bool olddirnaf = narf_find(olddir);
   bool newdirnaf = narf_find(newdir);

   if (!oldfile && !olddirnaf) {
      ret = -ENOENT;
      goto fini;
   }

   if (newfile || newdirnaf) {
      ret = -EEXIST;
      goto fini;
   }

   if (oldfile) {
      ret = 0;
      if (!narf_rename_key(oldpath + 1, newpath + 1)) {
         ret = -EIO;
      }
      goto fini;
   }

   // olddir is a directory.  this will be tricky.
   // this is only safe because i know what i'm doing...
   size_t olen = strlen(olddir);
   const char *entry2;
   char prevkey[512];
   char buf[512];
   entry2 = narf_dirfirst(olddir, "/");
   while (entry2 != NULL) {
      int n;

      n = snprintf(prevkey, sizeof(prevkey), "%s", entry2);
      if (n < 0 || (size_t) n >= sizeof(prevkey)) {
         ret = -ENAMETOOLONG;
         goto fini;
      }

      if (strlen(prevkey) < olen) {
         ret = -EIO;
         goto fini;
      }

      n = snprintf(buf, sizeof(buf), "%s%s", newdir, prevkey + olen);
      if (n < 0 || (size_t) n >= sizeof(buf)) {
         ret = -ENAMETOOLONG;
         goto fini;
      }

      if (!narf_rename_key(prevkey, buf)) {
         ret = -EIO;
         goto fini;
      }
      entry2 = narf_dirnext(olddir, "/", prevkey);
   }
   ret = 0;

fini:
   free(olddir);
   free(newdir);
   UNLOCK;
   return ret;
   // return -EROFS;
}

//! @brief FUSE link callback.
static int my_link(const char *from, const char *to) {
   (void) from;
   (void) to;

   if (!mounted) return -ENODEV;

   // Create a hard link
   return -EROFS;
}

typedef struct {
   mode_t type;
   mode_t mode;
} ChmodArg;

static void chmod_change(NarfFuseMeta *meta, void *arg) {
   ChmodArg *ch = (ChmodArg *) arg;

   meta->mode = ch->type | (ch->mode & 07777);
}

//! @brief FUSE chmod callback.
static int my_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
   char key[NARF_SECTOR_SIZE];
   bool is_dir;
   ChmodArg arg;
   int ret;

   (void) fi;

   if (!mounted) return -ENODEV;
   if (!strcmp(path, "/")) return -EPERM;

   LOCK;
   ret = key_for_existing_path(path, key, &is_dir);
   if (ret == 0) {
      arg.type = is_dir ? S_IFDIR : S_IFREG;
      arg.mode = mode;
      ret = commit_metadata_update(key, arg.type | (is_dir ? 0755 : 0644),
            chmod_change, &arg);
   }
   UNLOCK;

   return ret;
}

typedef struct {
   uid_t uid;
   gid_t gid;
} ChownArg;

static void chown_change(NarfFuseMeta *meta, void *arg) {
   ChownArg *ch = (ChownArg *) arg;

   if (ch->uid != (uid_t) -1) {
      meta->uid = ch->uid;
   }

   if (ch->gid != (gid_t) -1) {
      meta->gid = ch->gid;
   }
}

//! @brief FUSE chown callback.
static int my_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
   char key[NARF_SECTOR_SIZE];
   bool is_dir;
   ChownArg arg;
   mode_t default_mode;
   int ret;

   (void) fi;

   if (!mounted) return -ENODEV;
   if (!strcmp(path, "/")) return -EPERM;

   LOCK;
   ret = key_for_existing_path(path, key, &is_dir);
   if (ret == 0) {
      default_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
      arg.uid = uid;
      arg.gid = gid;
      ret = commit_metadata_update(key, default_mode, chown_change, &arg);
   }
   UNLOCK;

   return ret;
}

//! @brief FUSE truncate callback.
static int my_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
   char metadata[NARF_METADATA_SIZE];
   time_t mtime;
   int ret;

   (void) fi;

   if (!mounted) return -ENODEV;
   if (size < 0) return -EINVAL;
   if ((NarfByteSize) size != (uintmax_t) size) return -EFBIG;

   LOCK;

   if (!narf_find(path + 1)) {
      UNLOCK;
      return -ENOENT;
   }

   mtime = now_sec();
   ret = prepare_metadata_update(path + 1, S_IFREG | 0644,
         set_mtime_change, &mtime, metadata);
   if (ret != 0) {
      UNLOCK;
      return ret;
   }

   if (!narf_realloc(path + 1, (NarfByteSize) size)) {
      UNLOCK;
      return -EIO;
   }

   ret = write_metadata_string(path + 1, metadata);
   UNLOCK;
   return ret;
}

// --- File I/O ---
//! @brief FUSE open callback.
static int my_open(const char *path, struct fuse_file_info *fi) {
   (void) path;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Open a file
   // optional, fuse should only call if it exists.
   return 0;
}

//! @brief FUSE read callback.
static int my_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
   (void) fi;

   if (!mounted) return -ENODEV;
   if (offset < 0) return -EINVAL;

   LOCK;

   // Read data from a file

   if (!narf_find(path + 1)) {
      UNLOCK;
      return -ENOENT;
   }

   size_t len = narf_size(path + 1);
   NarfSector sector = narf_sector(path + 1);
   size_t read_offset = (size_t) offset;

   if (sector == INVALID_NAF || read_offset >= len) {
      UNLOCK;
      return 0;
   }

   if (size > len - read_offset) {
      size = len - read_offset;
   }

   if (size > INT_MAX) {
      UNLOCK;
      return -EFBIG;
   }

   // this would be so much easier if i cheated...

   while (read_offset >= NARF_SECTOR_SIZE) {
      read_offset -= NARF_SECTOR_SIZE;
      sector++;
   }

   size_t remaining = size;

   while (remaining) {
      char data[NARF_SECTOR_SIZE];
      if (!narf_io_read(sector, data)) {
         UNLOCK;
         return -EIO;
      }
      if ((size_t)(NARF_SECTOR_SIZE - read_offset) >= remaining) {
         memcpy(buf, data + read_offset, remaining);
         buf += remaining;
         remaining = 0;
      }
      else {
         memcpy(buf, data + read_offset, NARF_SECTOR_SIZE - read_offset);
         buf += (NARF_SECTOR_SIZE - read_offset);
         remaining -= (NARF_SECTOR_SIZE - read_offset);
         read_offset = 0;
      }
      sector++;
   }

   UNLOCK;
   return (int) size;
}

//! @brief FUSE write callback.
static int my_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
   char metadata[NARF_METADATA_SIZE];
   time_t mtime;
   int ret;

   (void) fi;

   if (!mounted) return -ENODEV;
   if (offset < 0) return -EINVAL;
   if ((NarfByteSize) offset != (uintmax_t) offset) return -EFBIG;
   if ((NarfByteSize) size != size) return -EFBIG;
   if ((NarfByteSize) size > ((NarfByteSize) -1) - (NarfByteSize) offset) return -EFBIG;
   if (size > INT_MAX) return -EFBIG;

   LOCK;

   // Write data to a file atomically by letting the core copy-on-write
   // the payload extent and commit metadata last.

   if (!narf_find(path + 1)) {
      UNLOCK;
      return -ENOENT;
   }

   mtime = now_sec();
   ret = prepare_metadata_update(path + 1, S_IFREG | 0644,
         set_mtime_change, &mtime, metadata);
   if (ret != 0) {
      UNLOCK;
      return ret;
   }

   if (!narf_write(path + 1, buf, (NarfByteSize) size, (NarfByteSize) offset)) {
      UNLOCK;
      return -EIO;
   }

   ret = write_metadata_string(path + 1, metadata);
   UNLOCK;

   if (ret != 0) {
      return ret;
   }

   return (int) size;
}

//! @brief FUSE statfs callback.
static int my_statfs(const char *path, struct statvfs *st) {
   (void) path;

   if (!mounted) return -ENODEV;

   LOCK;

   // Report filesystem stats
   NarfStat stats;
   if (!narf_stat(&stats)) {
      UNLOCK;
      return -EIO;
   }

   memset(st, 0, sizeof(*st));
   st->f_bsize = NARF_SECTOR_SIZE;
   st->f_frsize = NARF_SECTOR_SIZE;
   st->f_blocks = stats.total_sectors;
   st->f_bfree = stats.free_sectors;
   st->f_bavail = stats.free_sectors;
   st->f_files = stats.file_count;
   st->f_ffree = stats.free_sectors / 2;
   st->f_namemax = stats.max_key_bytes;

   UNLOCK;
   return 0;
}

//! @brief FUSE flush callback.
static int my_flush(const char *path, struct fuse_file_info *fi) {
   (void) path;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Flush file contents (can often be a no-op)
   return 0;
}

//! @brief FUSE release callback.
static int my_release(const char *path, struct fuse_file_info *fi) {
   (void) path;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Close file
   // optional in our case
   return 0;
}

//! @brief FUSE fsync callback.
static int my_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
   (void) path;
   (void) isdatasync;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Flush file to storage
   return 0;
}

// --- Directory handling ---
//! @brief FUSE opendir callback.
static int my_opendir(const char *path, struct fuse_file_info *fi) {
   (void) path;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Open directory
   return 0;
}

static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
      off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
   (void) offset;
   (void) fi;
   (void) flags;

   if (!mounted) return -ENODEV;

   // List contents of directory

   LOCK;

   filler(buf, ".", NULL, 0, 0);
   filler(buf, "..", NULL, 0, 0);

   if (!strcmp(path, "/")) {
      const char *entry = narf_dirfirst("", "/");
      while (entry != NULL) {
         if (*entry) {
            size_t entry_len = strlen(entry);

            if (entry[entry_len - 1] == '/') {
               char *dirname = strdup(entry);

               if (dirname == NULL) {
                  UNLOCK;
                  return -ENOMEM;
               }

               dirname[entry_len - 1] = 0;
               filler(buf, dirname, NULL, 0, 0);
               free(dirname);
            }
            else {
               filler(buf, entry, NULL, 0, 0);
            }
         }

         entry = narf_dirnext("", "/", entry);
      }
   }
   else {
      char *p = xformpath(path);
      const char *entry = narf_dirfirst(p, "/");
      while (entry != NULL) {
         const char *q = entry + strlen(p);
         if (*q) {
            size_t q_len = strlen(q);

            if (q[q_len - 1] == '/') {
               char *dirname = strdup(q);

               if (dirname == NULL) {
                  free(p);
                  UNLOCK;
                  return -ENOMEM;
               }

               dirname[q_len - 1] = 0;
               filler(buf, dirname, NULL, 0, 0);
               free(dirname);
            }
            else {
               filler(buf, q, NULL, 0, 0);
            }
         }
         entry = narf_dirnext(p, "/", entry);
      }
      free(p);
   }

   UNLOCK;
   return 0;
}

//! @brief FUSE releasedir callback.
static int my_releasedir(const char *path, struct fuse_file_info *fi) {
   (void) path;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Close directory
   return 0;
}

//! @brief FUSE fsyncdir callback.
static int my_fsyncdir(const char *path, int isdatasync, struct fuse_file_info *fi) {
   (void) path;
   (void) isdatasync;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Sync directory to disk
   return 0;
}

// --- File creation ---
//! @brief FUSE create callback.
static int my_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
   int ret;

   (void) fi;

   if (!mounted) return -ENODEV;

   LOCK;

   // Create and open a file
   if (narf_find(path + 1)) {
      // it already exists
      UNLOCK;
      return 0;
   }

   if (!narf_alloc(path + 1, 0)) {
      UNLOCK;
      return -EROFS;
   }

   ret = init_metadata_for_key(path + 1, S_IFREG | (mode & 07777));
   if (ret != 0) {
      narf_free_key(path + 1);
      UNLOCK;
      return ret;
   }

   UNLOCK;
   return 0;
}

// --- Time update ---
//! @brief FUSE utimens callback.
static int my_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
   char key[NARF_SECTOR_SIZE];
   bool is_dir;
   mode_t default_mode;
   time_t mtime;
   int ret;

   (void) fi;

   if (!mounted) return -ENODEV;
   if (!strcmp(path, "/")) return 0;

   if (tv == NULL || tv[1].tv_nsec == UTIME_NOW) {
      mtime = now_sec();
   }
   else if (tv[1].tv_nsec == UTIME_OMIT) {
      return 0;
   }
   else {
      mtime = tv[1].tv_sec;
   }

   LOCK;
   ret = key_for_existing_path(path, key, &is_dir);
   if (ret == 0) {
      default_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
      ret = commit_metadata_update(key, default_mode, set_mtime_change, &mtime);
   }
   UNLOCK;

   return ret;
}

// --- Block map (optional) ---
//! @brief FUSE bmap callback.
static int my_bmap(const char *path, size_t blocksize, uint64_t *idx) {
   (void) path;
   (void) blocksize;
   (void) idx;

   if (!mounted) return -ENODEV;

   // Map logical block to physical (rarely used)
   return -ENOSYS;
}

// --- Extended attributes (optional) ---
//! @brief FUSE setxattr callback.
static int my_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
   char key[NARF_SECTOR_SIZE];
   char old_metadata[NARF_METADATA_SIZE];
   char existing[NARF_METADATA_SIZE];
   char xvalue[NARF_METADATA_SIZE];
   bool is_dir;
   const char *xkey;
   int ret;

   if (!mounted) return -ENODEV;
   if (!strcmp(path, "/")) return -EPERM;
   if ((flags & XATTR_CREATE) && (flags & XATTR_REPLACE)) return -EINVAL;
   if (value == NULL && size != 0) return -EINVAL;

   ret = split_user_xattr(name, &xkey);
   if (ret != 0) {
      return ret;
   }

   if (size >= sizeof(xvalue)) {
      return -ENOSPC;
   }

   memcpy(xvalue, value, size);
   xvalue[size] = 0;

   if (!custom_meta_value_ok(xvalue)) {
      return -EINVAL;
   }

   LOCK;
   ret = key_for_existing_path(path, key, &is_dir);
   if (ret == 0) {
      NarfFuseMeta meta;
      mode_t default_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);

      ret = read_metadata(key, default_mode, &meta, old_metadata);
      if (ret == 0) {
         bool found = find_custom_value(old_metadata, xkey, existing);

         if ((flags & XATTR_CREATE) && found) {
            ret = -EEXIST;
         }
         else if ((flags & XATTR_REPLACE) && !found) {
            ret = -ENODATA;
         }
         else {
            ret = set_custom_value(key, is_dir, xkey, xvalue, false);
         }
      }
   }
   UNLOCK;

   return ret;
}

//! @brief FUSE getxattr callback.
static int my_getxattr(const char *path, const char *name, char *value, size_t size) {
   char key[NARF_SECTOR_SIZE];
   char metadata[NARF_METADATA_SIZE];
   char xvalue[NARF_METADATA_SIZE];
   bool is_dir;
   NarfFuseMeta meta;
   const char *xkey;
   size_t len;
   int ret;

   if (!mounted) return -ENODEV;

   ret = split_user_xattr(name, &xkey);
   if (ret != 0) {
      return ret;
   }

   if (!strcmp(path, "/")) {
      return -ENODATA;
   }

   LOCK;
   ret = metadata_for_path(path, key, &is_dir, &meta, metadata);
   UNLOCK;

   if (ret != 0) {
      return ret;
   }

   (void) key;
   (void) is_dir;
   (void) meta;

   if (!find_custom_value(metadata, xkey, xvalue)) {
      return -ENODATA;
   }

   len = strlen(xvalue);

   if (size == 0) {
      return (int) len;
   }

   if (size < len) {
      return -ERANGE;
   }

   memcpy(value, xvalue, len);
   return (int) len;
}

static int list_append_xattr(char *list, size_t size, size_t *used,
      const char *key) {
   size_t name_len = strlen(NARF_XATTR_PREFIX) + strlen(key) + 1;

   if (list != NULL && size < *used + name_len) {
      return -ERANGE;
   }

   if (list != NULL) {
      snprintf(list + *used, size - *used, "%s%s", NARF_XATTR_PREFIX, key);
   }

   *used += name_len;
   return 0;
}

//! @brief FUSE listxattr callback.
static int my_listxattr(const char *path, char *list, size_t size) {
   char key[NARF_SECTOR_SIZE];
   char metadata[NARF_METADATA_SIZE];
   char tmp[NARF_METADATA_SIZE];
   bool is_dir;
   NarfFuseMeta meta;
   char *save = NULL;
   char *token;
   size_t used = 0;
   int ret;

   if (!mounted) return -ENODEV;

   if (size == 0) {
      list = NULL;
   }

   if (!strcmp(path, "/")) {
      return 0;
   }

   LOCK;
   ret = metadata_for_path(path, key, &is_dir, &meta, metadata);
   UNLOCK;

   if (ret != 0) {
      return ret;
   }

   (void) key;
   (void) is_dir;
   (void) meta;

   snprintf(tmp, sizeof(tmp), "%s", metadata);
   token = strtok_r(tmp, " ", &save);

   if (token == NULL || strcmp(token, NARF_META_VERSION)) {
      return 0;
   }

   while ((token = strtok_r(NULL, " ", &save)) != NULL) {
      char *eq = strchr(token, '=');
      char *xkey;
      char *xvalue;

      if (eq == NULL) {
         continue;
      }

      *eq = 0;
      xkey = token;
      xvalue = eq + 1;

      if (reserved_meta_key(xkey) || !custom_meta_key_ok(xkey) ||
            !custom_meta_value_ok(xvalue)) {
         continue;
      }

      ret = list_append_xattr(list, size, &used, xkey);
      if (ret != 0) {
         return ret;
      }
   }

   return (int) used;
}

//! @brief FUSE removexattr callback.
static int my_removexattr(const char *path, const char *name) {
   char key[NARF_SECTOR_SIZE];
   char metadata[NARF_METADATA_SIZE];
   char value[NARF_METADATA_SIZE];
   bool is_dir;
   NarfFuseMeta meta;
   const char *xkey;
   int ret;

   if (!mounted) return -ENODEV;
   if (!strcmp(path, "/")) return -ENODATA;

   ret = split_user_xattr(name, &xkey);
   if (ret != 0) {
      return ret;
   }

   LOCK;
   ret = metadata_for_path(path, key, &is_dir, &meta, metadata);
   if (ret == 0) {
      if (!find_custom_value(metadata, xkey, value)) {
         ret = -ENODATA;
      }
      else {
         ret = set_custom_value(key, is_dir, xkey, NULL, true);
      }
   }
   UNLOCK;

   return ret;
}

// --- Filesystem lifecycle ---
static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
   (void) conn;
   (void) cfg;

   // Called on mount
   mount_time = now_sec();
   LOCK;
   if (partition == -1) {
      mounted = narf_init(0);
   }
   else {
      if (partition == 0) {
         partition = narf_findpart();
      }
      mounted = narf_mount(partition);
   }
   UNLOCK;
   return NULL;
}

//! @brief FUSE destroy callback.
static void my_destroy(void *private_data) {
   (void) private_data;

   //if (!mounted) return -ENODEV;

   // Called on unmount
}

static struct fuse_operations my_ops = {
   .getattr     = my_getattr,
   .readlink    = my_readlink,
   .mknod       = my_mknod,
   .mkdir       = my_mkdir,
   .unlink      = my_unlink,
   .rmdir       = my_rmdir,
   .symlink     = my_symlink,
   .rename      = my_rename,
   .link        = my_link,
   .chmod       = my_chmod,
   .chown       = my_chown,
   .truncate    = my_truncate,
   .open        = my_open,
   .read        = my_read,
   .write       = my_write,
   .statfs      = my_statfs,
   .flush       = my_flush,
   .release     = my_release,
   .fsync       = my_fsync,
   .opendir     = my_opendir,
   .readdir     = my_readdir,
   .releasedir  = my_releasedir,
   .fsyncdir    = my_fsyncdir,
   .create      = my_create,
   .access      = my_access,
   .utimens     = my_utimens,
   .bmap        = my_bmap,
   .setxattr    = my_setxattr,
   .getxattr    = my_getxattr,
   .listxattr   = my_listxattr,
   .removexattr = my_removexattr,
   .init        = my_init,
   .destroy     = my_destroy,
};

//! @brief Print FUSE front-end usage help.
void usage(const char *progname) {
    fprintf(stderr,
        "Usage: %s <backing_file[:N]> [FUSE options...]\n"
        "  <backing_file> : raw device or image file\n"
        "  [:N]           : optional partition number to mount\n"
        "                  - if : is present but no number, will auto-detect 0x6E type\n",
        progname);
    exit(1);
}

//! @brief Parse arguments and start the FUSE mount.
int main(int argc, char *argv[]) {
   if (argc < 3) {
      usage(argv[0]);
   }

   char *filename = argv[1];
   char *colon = strchr(filename, ':');

   if (colon) {
      *colon = 0;
      colon++;
      if (*colon) {
         partition = *colon - '0';
      }
      else {
         partition = 0;
      }
   }

   fd = open(filename, O_RDWR);
   if (fd < 0) {
      perror("open existing");
      return 1;
   }
   size = lseek(fd, 0, SEEK_END);

   argv[1] = argv[0];
   argc--;
   argv++;
   return fuse_main(argc, argv, &my_ops, NULL);
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
