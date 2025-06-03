#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "narf_conf.h"
#include "narf_io.h"
#include "narf.h"

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
   if (path[0] == '/') {
      path++;
   }
   int n = strlen(path);
   char *p = malloc(n + 2);
   strcpy(p, path);
   if (p[strlen(p) - 1] != '/') {
      strcat(p, "/");
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
   off_t off = lseek(fd, sector * NARF_SECTOR_SIZE, SEEK_SET);
   if (off == -1) { return false; }
   ssize_t size = write(fd, data, NARF_SECTOR_SIZE);
   if (size != NARF_SECTOR_SIZE) { return false; }
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
   off_t off = lseek(fd, sector * NARF_SECTOR_SIZE, SEEK_SET);
   if (off == -1) { return false; }
   ssize_t size = read(fd, data, NARF_SECTOR_SIZE);
   if (size != NARF_SECTOR_SIZE) { return false; }
   fsync(fd);
   return true;
}

// --- File & directory metadata ---
static int my_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
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
   NAF naf = narf_find(path + 1);
   if (naf != INVALID_NAF) {
      st->st_mode = S_IFREG | 0444;
      st->st_nlink = 1;
      st->st_size = narf_size(naf);
      UNLOCK;
      return 0;
   }

   // see if it exists as a directory
   char *p = xformpath(path);
   naf = narf_find(p);
   if (naf != INVALID_NAF) {
      free(p);
      st->st_mode = S_IFDIR | 0755;
      st->st_nlink = 2;
      st->st_size = narf_size(naf);
      UNLOCK;
      return 0;
   }

   // see if it exists as a phantom directory
   naf = narf_dirfirst(p, "/");
   free(p);
   if (naf != INVALID_NAF) {
      st->st_mode = S_IFDIR | 0755;
      st->st_nlink = 2;
      st->st_size = 0;
      UNLOCK;
      return 0;
   }

   UNLOCK;
   return -ENOENT;
}

static int my_access(const char *path, int mask) {
   if (!mounted) return -ENODEV;

   // Check file permissions
   return 0; // everything allowed
             // return -EACCES; // denied
}

static int my_readlink(const char *path, char *buf, size_t size) {
   if (!mounted) return -ENODEV;

   // Return symlink target
   return -EINVAL;
}

static int my_mknod(const char *path, mode_t mode, dev_t rdev) {
   if (!mounted) return -ENODEV;

   // Create special files (FIFO, char/block dev, etc.)
   return -EPERM;
}

static int my_mkdir(const char *path, mode_t mode) {
   if (!mounted) return -ENODEV;

   LOCK;

   // Create a directory
   if (narf_find(path + 1) != INVALID_NAF) {
      // it already exists as a file
      UNLOCK;
      return -EEXIST;
   }

   char *p = xformpath(path);

   if (narf_find(p) != INVALID_NAF) {
      // it already exists as a directory
      free(p);
      UNLOCK;
      return -EEXIST;
   }
   else if (narf_alloc(p, 0) != INVALID_NAF) {
      // we had to create it
      free(p);
      UNLOCK;
      return 0;
   }
      UNLOCK;
   return -EIO;
   // return -EROFS;
}

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

static int my_rmdir(const char *path) {
   if (!mounted) return -ENODEV;

   LOCK;

   // Remove a directory

   if (narf_find(path + 1) != INVALID_NAF) {
      UNLOCK;
      return -ENOTDIR;
   }

   char *p = xformpath(path);
   NAF naf = narf_dirfirst(p, "/");

   if (naf == INVALID_NAF) {
      free(p);
      UNLOCK;
      return -ENOENT;
   }

   if (strcmp(p, narf_key(naf))) {
      free(p);
      UNLOCK;
      return -ENOTEMPTY;
   }

   naf = narf_dirnext(p, "/", naf);

   if (naf != INVALID_NAF) {
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

static int my_symlink(const char *target, const char *linkpath) {
   if (!mounted) return -ENODEV;

   // Create a symbolic link
   return -EROFS;
}

static int my_rename(const char *oldpath, const char *newpath, unsigned int flags) {
   if (!mounted) return -ENODEV;

   // Rename or move file/directory

   LOCK;

   int ret = -EROFS;

   char *olddir = xformpath(oldpath);
   char *newdir = xformpath(newpath);

   NAF oldfile = narf_find(oldpath + 1);
   NAF newfile = narf_find(newpath + 1);
   NAF olddirnaf = narf_find(olddir);
   NAF newdirnaf = narf_find(newdir);

   if (oldfile == INVALID_NAF && olddirnaf == INVALID_NAF) {
      ret = -ENOENT;
      goto fini;
   }

   if (newfile != INVALID_NAF || newdirnaf != INVALID_NAF) {
      ret = -EEXIST;
      goto fini;
   }

   if (oldfile != INVALID_NAF) {
      ret = 0;
      if (!narf_rename_key(oldpath + 1, newpath + 1)) {
         ret = -EIO;
      }
      goto fini;
   }

   // olddir is a directory.  this will be tricky.
   // this is only safe because i know what i'm doing...
   int olen = strlen(olddir);
   int nlen = strlen(newdir);
   NAF naf, nextnaf;
   for (naf = narf_find(olddir);
         naf != INVALID_NAF;
         naf = nextnaf) {
      nextnaf = narf_next(naf);
      char buf[512];
      char *x = strdup(narf_key(naf));
      if (!strncmp(olddir, x, olen)) {
         strcpy(buf + sizeof(buf) - 1 - strlen(x), x);
         memcpy(buf + sizeof(buf) - 1 - strlen(x) + olen - nlen, newdir, nlen);
         if (!narf_rename_key(x, buf + sizeof(buf) - 1 - strlen(x) + olen - nlen)) {
            free(x);
            ret = -EIO;
            goto fini;
         }
         free(x);
      }
      else {
         break;
      }
   }
   ret = 0;

fini:
   free(olddir);
   free(newdir);
   UNLOCK;
   return ret;
   // return -EROFS;
}

static int my_link(const char *from, const char *to) {
   if (!mounted) return -ENODEV;

   // Create a hard link
   return -EROFS;
}

static int my_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Change permissions
   return -EPERM;
}

static int my_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Change owner/group
   return -EPERM;
}

static int my_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Resize file
   return -EROFS;
}

// --- File I/O ---
static int my_open(const char *path, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Open a file
   // optional, fuse should only call if it exists.
   return 0;
}

static int my_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   LOCK;

   // Read data from a file

   NAF naf = narf_find(path + 1);
   if (naf == INVALID_NAF) {
      UNLOCK;
      return -ENOENT;
   }

   size_t len = narf_size(naf);
   naf = narf_sector(naf);

   if (naf == INVALID_NAF || offset >= len) {
      UNLOCK;
      return 0;
   }

   if (offset + size > len) {
      size = len - offset;
   }

   // this would be so much easier if i cheated...

   while (offset >= NARF_SECTOR_SIZE) {
      offset -= NARF_SECTOR_SIZE;
      naf++;
   }

   size_t remaining = size;

   while (remaining) {
      char data[NARF_SECTOR_SIZE];
      if (!narf_io_read(naf, data)) {
         UNLOCK;
         return -EIO;
      }
      if (NARF_SECTOR_SIZE - offset >= remaining) {
         memcpy(buf, data + offset, remaining);
         buf += remaining;
         remaining = 0;
      }
      else {
         memcpy(buf, data + offset, NARF_SECTOR_SIZE - offset);
         buf += (NARF_SECTOR_SIZE - offset);
         remaining -= (NARF_SECTOR_SIZE - offset);
         offset = 0;
      }
      naf++;
   }

   UNLOCK;
   return size;
}

static int my_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   LOCK;

   // Write data to a file

   NAF naf = narf_find(path + 1);

   if (naf == INVALID_NAF) {
      UNLOCK;
      return -ENOENT;
   }

   size_t len = narf_size(naf);

   if (len < offset + size) {
      naf = narf_realloc(naf, offset + size);
      if (naf == INVALID_NAF) {
         UNLOCK;
         return -EIO;
      }
      len = offset + size;
   }

   naf = narf_sector(naf);

   while (offset >= NARF_SECTOR_SIZE) {
      offset -= NARF_SECTOR_SIZE;
      naf++;
   }

   size_t remaining = size;

   while (remaining) {
      char data[NARF_SECTOR_SIZE];
      if (!narf_io_read(naf, data)) {
         UNLOCK;
         return -EIO;
      }
      if (NARF_SECTOR_SIZE - offset >= remaining) {
         memcpy(data + offset, buf, remaining);
         buf += remaining;
         remaining = 0;
      }
      else {
         memcpy(data + offset, buf, NARF_SECTOR_SIZE - offset);
         buf += (NARF_SECTOR_SIZE - offset);
         remaining -= (NARF_SECTOR_SIZE - offset);
         offset = 0;
      }
      if (!narf_io_write(naf, data)) {
         UNLOCK;
         return -EIO;
      }
      naf++;
   }

   UNLOCK;
   return size;
   //return -EROFS;
}

static int my_statfs(const char *path, struct statvfs *st) {
   if (!mounted) return -ENODEV;

   // Report filesystem stats
   memset(st, 0, sizeof(*st));
   return 0;
}

static int my_flush(const char *path, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Flush file contents (can often be a no-op)
   return 0;
}

static int my_release(const char *path, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Close file
   // optional in our case
   return 0;
}

static int my_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Flush file to storage
   return 0;
}

// --- Directory handling ---
static int my_opendir(const char *path, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Open directory
   return 0;
}

static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
      off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
   if (!mounted) return -ENODEV;

   // List contents of directory

   LOCK;

   filler(buf, ".", NULL, 0, 0);
   filler(buf, "..", NULL, 0, 0);

   if (!strcmp(path, "/")) {
      NAF naf;
      char *p = NULL, *q = NULL, *r = NULL;

      for (naf = narf_first(); naf != INVALID_NAF; naf = narf_next(naf)) {
         p = strdup(narf_key(naf));
         r = strchr(p, '/');
         if (r == NULL) {
            // just add it
            filler(buf, p, NULL, 0, 0);
            if (q) {
               free(q);
               q = NULL;
            }
            free(p);
         }
         else {
            *r = 0;
            if (!q || strcmp(p, q)) {
               // a new one!
               if (q) {
                  free(q);
               }
               q = p;
               filler(buf, p, NULL, 0, 0);
            }
            else {
               // seen it!
               free(p);
            }
         }
      }
   }
   else {
      char *p = xformpath(path);

      NAF naf = narf_dirfirst(p, "/");

      if (naf != INVALID_NAF) {
         while (naf != INVALID_NAF) {
            const char *q = narf_key(naf) + strlen(p);
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
            naf = narf_dirnext(p, "/", naf);
         }
      }

      free(p);
   }

   UNLOCK;
   return 0;
}

static int my_releasedir(const char *path, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Close directory
   return 0;
}

static int my_fsyncdir(const char *path, int isdatasync, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Sync directory to disk
   return 0;
}

// --- File creation ---
static int my_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   LOCK;

   // Create and open a file
   if (narf_find(path+1) != INVALID_NAF) {
      // it already exists
      UNLOCK;
      return 0;
   }
   else if (narf_alloc(path+1, 0) != INVALID_NAF) {
      // we had to create it
      UNLOCK;
      return 0;
   }
   UNLOCK;
   return -EROFS;
}

// --- Time update ---
static int my_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
   if (!mounted) return -ENODEV;

   // Update file access/modification times
   return 0;
}

// --- Block map (optional) ---
static int my_bmap(const char *path, size_t blocksize, uint64_t *idx) {
   if (!mounted) return -ENODEV;

   // Map logical block to physical (rarely used)
   return -ENOSYS;
}

// --- Extended attributes (optional) ---
static int my_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
   if (!mounted) return -ENODEV;

   return -ENOTSUP;
}

static int my_getxattr(const char *path, const char *name, char *value, size_t size) {
   if (!mounted) return -ENODEV;

   return -ENOTSUP;
}

static int my_listxattr(const char *path, char *list, size_t size) {
   if (!mounted) return -ENODEV;

   return -ENOTSUP;
}

static int my_removexattr(const char *path, const char *name) {
   if (!mounted) return -ENODEV;

   return -ENOTSUP;
}

// --- Filesystem lifecycle ---
static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
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

static void my_destroy(void *private_data) {
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

void usage(const char *progname) {
    fprintf(stderr,
        "Usage: %s <backing_file[:N]> [FUSE options...]\n"
        "  <backing_file> : raw device or image file\n"
        "  [:N]           : optional partition number to mount\n"
        "                  - if : is present but no number, will auto-detect 0x6E type\n",
        progname);
    exit(1);
}

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
   size = lseek(fd, SEEK_END, 0);

   argv[1] = argv[0];
   argc--;
   argv++;
   return fuse_main(argc, argv, &my_ops, NULL);
}

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
