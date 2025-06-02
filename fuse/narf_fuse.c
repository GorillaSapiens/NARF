#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

// --- File & directory metadata ---
static int my_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    // Fill in stat structure for a file or directory
    return -ENOENT;
}

static int my_access(const char *path, int mask) {
    // Check file permissions
    return -EACCES;
}

static int my_readlink(const char *path, char *buf, size_t size) {
    // Return symlink target
    return -EINVAL;
}

static int my_mknod(const char *path, mode_t mode, dev_t rdev) {
    // Create special files (FIFO, char/block dev, etc.)
    return -EPERM;
}

static int my_mkdir(const char *path, mode_t mode) {
    // Create a directory
    return -EROFS;
}

static int my_unlink(const char *path) {
    // Delete a file
    return -EROFS;
}

static int my_rmdir(const char *path) {
    // Remove a directory
    return -EROFS;
}

static int my_symlink(const char *target, const char *linkpath) {
    // Create a symbolic link
    return -EROFS;
}

static int my_rename(const char *oldpath, const char *newpath, unsigned int flags) {
    // Rename or move file/directory
    return -EROFS;
}

static int my_link(const char *from, const char *to) {
    // Create a hard link
    return -EROFS;
}

static int my_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    // Change permissions
    return -EPERM;
}

static int my_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    // Change owner/group
    return -EPERM;
}

static int my_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    // Resize file
    return -EROFS;
}

// --- File I/O ---
static int my_open(const char *path, struct fuse_file_info *fi) {
    // Open a file
    return 0;
}

static int my_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    // Read data from a file
    return 0;
}

static int my_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    // Write data to a file
    return -EROFS;
}

static int my_statfs(const char *path, struct statvfs *st) {
    // Report filesystem stats
    memset(st, 0, sizeof(*st));
    return 0;
}

static int my_flush(const char *path, struct fuse_file_info *fi) {
    // Flush file contents (can often be a no-op)
    return 0;
}

static int my_release(const char *path, struct fuse_file_info *fi) {
    // Close file
    return 0;
}

static int my_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
    // Flush file to storage
    return 0;
}

// --- Directory handling ---
static int my_opendir(const char *path, struct fuse_file_info *fi) {
    // Open directory
    return 0;
}

static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    // List contents of directory
    return 0;
}

static int my_releasedir(const char *path, struct fuse_file_info *fi) {
    // Close directory
    return 0;
}

static int my_fsyncdir(const char *path, int isdatasync, struct fuse_file_info *fi) {
    // Sync directory to disk
    return 0;
}

// --- File creation ---
static int my_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    // Create and open a file
    return -EROFS;
}

// --- Time update ---
static int my_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    // Update file access/modification times
    return 0;
}

// --- Block map (optional) ---
static int my_bmap(const char *path, size_t blocksize, uint64_t *idx) {
    // Map logical block to physical (rarely used)
    return -ENOSYS;
}

// --- Extended attributes (optional) ---
static int my_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
    return -ENOTSUP;
}

static int my_getxattr(const char *path, const char *name, char *value, size_t size) {
    return -ENOTSUP;
}

static int my_listxattr(const char *path, char *list, size_t size) {
    return -ENOTSUP;
}

static int my_removexattr(const char *path, const char *name) {
    return -ENOTSUP;
}

// --- Filesystem lifecycle ---
static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    // Called on mount
    return NULL;
}

static void my_destroy(void *private_data) {
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

int main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &my_ops, NULL);
}
