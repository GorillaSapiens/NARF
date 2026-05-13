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

   // Fill in stat structure for a file or directory
   memset(st, 0, sizeof(*st));

   // root always exists
   if (strcmp(path, "/") == 0) {
      st->st_mode = S_IFDIR | 0755;
      st->st_nlink = 2;
      st->st_size = 0;
      UNLOCK;
      return 0;
   }

   // see if it exists as a file
   if (narf_find(path + 1)) {
      st->st_mode = S_IFREG | 0444;
      st->st_nlink = 1;
      st->st_size = narf_size(path + 1);
      UNLOCK;
      return 0;
   }

   // see if it exists as a directory
   char *p = xformpath(path);
   if (narf_find(p)) {
      st->st_mode = S_IFDIR | 0755;
      st->st_nlink = 2;
      st->st_size = narf_size(p);
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
      st->st_size = 0;
      UNLOCK;
      return 0;
   }

   UNLOCK;
   return -ENOENT;
}

//! @brief FUSE access callback.
static int my_access(const char *path, int mask) {
   (void) path;
   (void) mask;

   if (!mounted) return -ENODEV;

   // Check file permissions
   return 0; // everything allowed
             // return -EACCES; // denied
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
   (void) mode;

   if (!mounted) return -ENODEV;

   LOCK;

   // Create a directory
   if (narf_find(path + 1)) {
      // it already exists as a file
      UNLOCK;
      return -EEXIST;
   }

   char *p = xformpath(path);

   if (narf_find(p)) {
      // it already exists as a directory
      free(p);
      UNLOCK;
      return -EEXIST;
   }
   else if (narf_alloc(p, 0)) {
      // we had to create it
      free(p);
      UNLOCK;
      return 0;
   }
      UNLOCK;
   return -EIO;
   // return -EROFS;
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

//! @brief FUSE chmod callback.
static int my_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
   (void) path;
   (void) mode;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Change permissions
   return -EPERM;
}

//! @brief FUSE chown callback.
static int my_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
   (void) path;
   (void) uid;
   (void) gid;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Change owner/group
   return -EPERM;
}

//! @brief FUSE truncate callback.
static int my_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
   (void) fi;

   if (!mounted) return -ENODEV;
   if (size < 0) return -EINVAL;
   if ((NarfByteSize) size != (uintmax_t) size) return -EFBIG;

   LOCK;

   // Resize file
   if (!narf_find(path + 1)) {
      UNLOCK;
      return -ENOENT;
   }

   if (!narf_realloc(path + 1, (NarfByteSize) size)) {
      UNLOCK;
      return -EIO;
   }

   UNLOCK;
   return 0;
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

   if (!narf_write(path + 1, buf, (NarfByteSize) size, (NarfByteSize) offset)) {
      UNLOCK;
      return -EIO;
   }

   UNLOCK;
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
         const char *slash = strchr(entry, '/');
         if (slash == NULL) {
            filler(buf, entry, NULL, 0, 0);
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
            if (q[strlen(q) - 1] == '/') {
               char *dirname = strdup(q);
               dirname[strlen(dirname) - 1] = 0;
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
   (void) mode;
   (void) fi;

   if (!mounted) return -ENODEV;

   LOCK;

   // Create and open a file
   if (narf_find(path+1)) {
      // it already exists
      UNLOCK;
      return 0;
   }
   else if (narf_alloc(path+1, 0)) {
      // we had to create it
      UNLOCK;
      return 0;
   }
   UNLOCK;
   return -EROFS;
}

// --- Time update ---
//! @brief FUSE utimens callback.
static int my_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
   (void) path;
   (void) tv;
   (void) fi;

   if (!mounted) return -ENODEV;

   // Update file access/modification times
   return 0;
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
   (void) path;
   (void) name;
   (void) value;
   (void) size;
   (void) flags;

   if (!mounted) return -ENODEV;

   return -ENOTSUP;
}

//! @brief FUSE getxattr callback.
static int my_getxattr(const char *path, const char *name, char *value, size_t size) {
   (void) path;
   (void) name;
   (void) value;
   (void) size;

   if (!mounted) return -ENODEV;

   return -ENOTSUP;
}

//! @brief FUSE listxattr callback.
static int my_listxattr(const char *path, char *list, size_t size) {
   (void) path;
   (void) list;
   (void) size;

   if (!mounted) return -ENODEV;

   return -ENOTSUP;
}

//! @brief FUSE removexattr callback.
static int my_removexattr(const char *path, const char *name) {
   (void) path;
   (void) name;

   if (!mounted) return -ENODEV;

   return -ENOTSUP;
}

// --- Filesystem lifecycle ---
static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
   (void) conn;
   (void) cfg;

   // Called on mount
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
