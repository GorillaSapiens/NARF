NARF FUSE quick start
=====================

This document shows how to create a NARF filesystem image with the interactive
`narf_tester` program, mount it with the FUSE driver, and unmount it cleanly.

The commands below assume you are in the top-level NARF source directory.


1. Install build/runtime dependencies
-------------------------------------

On Debian or Ubuntu, install FUSE 3 development support:

   sudo apt install build-essential pkg-config fuse3 libfuse3-dev

If you are using local `.deb` files instead of packages from a repository, install
those instead, for example:

   sudo apt install ./fuse3_*.deb ./libfuse3-dev_*.deb

Make sure `/dev/fuse` exists and your user is allowed to use FUSE.  On a normal
Linux desktop this is already handled.  In containers, FUSE may not be available
or mounting may be blocked by container policy.


2. Build narf_tester and the FUSE tools
----------------------------------

From the top-level source directory:

   make

Expected outputs:

   src/narf_tester
   src/narf_fuse
   src/narf_mkfs

You can also build pieces directly:

   cd src
   make
   cd ..


3. Create a whole-image NARF filesystem with narf_tester
---------------------------------------------------

This creates a 16 MiB image named `narf.img`, formats the whole image as NARF,
mounts it inside narf_tester, creates a few sample keys, lists the root, and exits.

`narf_tester` commands use NARF keys directly.  Do not put a leading `/` on those keys;
`dir/readme.txt` is the stored key.  The FUSE mount still presents normal Unix
paths such as `/dir/readme.txt` to applications.

   ./src/narf_tester =16M,narf.img <<'EOF'
   mkfs
   init
   create hello.txt "hello from NARF\n"
   create dir/readme.txt "this file is inside a pseudo-directory\n"
   ls /
   ls /dir/
   quit
   EOF

The important narf_tester commands for a whole-image filesystem are:

   mkfs      format the whole image starting at sector 0
   init      mount/initialize the whole-image filesystem inside narf_tester
   quit      close narf_tester before using the image with FUSE

Do not leave narf_tester running while mounting the same image through FUSE.  NARF is
not designed for two independent writers at the same time.


4. Mount the whole-image filesystem with FUSE
---------------------------------------------

Create a host mount point:

   mkdir -p mnt-narf

Mount the image:

   ./src/narf_fuse narf.img mnt-narf

Now use ordinary shell commands against the mounted tree:

   ls -la mnt-narf
   cat mnt-narf/hello.txt
   cat mnt-narf/dir/readme.txt

To keep the FUSE process in the foreground, add `-f` after the mount point:

   ./src/narf_fuse narf.img mnt-narf -f

That is useful for debugging.  Open another terminal to inspect or unmount it.


5. Unmount the FUSE filesystem
------------------------------

Use `fusermount3`:

   fusermount3 -u mnt-narf

If your distribution uses the generic unmount command for FUSE filesystems, this
also usually works:

   umount mnt-narf

If a command says the mount point is busy, leave any shell whose current
directory is inside `mnt-narf`, close programs using files there, and retry.

If a crashed FUSE process leaves a stale mount behind, this often clears it:

   fusermount3 -uz mnt-narf

The `-z` option is lazy unmount.  Use it only for cleanup, not as the normal
happy path.


6. Optional: create an MBR-partitioned image with narf_tester
-------------------------------------------------------

The FUSE driver can also mount a NARF partition from an image with an MBR.
This creates a 16 MiB image, writes an MBR, creates partition 1, formats it,
mounts partition 1 inside narf_tester, and creates one file.

   ./src/narf_tester =16M,narf-part.img <<'EOF'
   mbr
   partition 1
   format 1
   mount 1
   create hello.txt "hello from NARF partition 1\n"
   ls /
   quit
   EOF

Mount partition 1 explicitly:

   mkdir -p mnt-narf
   ./src/narf_fuse narf-part.img:1 mnt-narf

Or ask the FUSE driver to auto-detect the first NARF partition by using a colon
with no number:

   ./src/narf_fuse narf-part.img: mnt-narf

Unmount the same way:

   fusermount3 -u mnt-narf


7. Useful FUSE options
----------------------

Pass normal FUSE options after the mount point:

   ./src/narf_fuse narf.img mnt-narf -f
   ./src/narf_fuse narf.img mnt-narf -d
   ./src/narf_fuse narf.img mnt-narf -f -d

Common options:

   -f      run in foreground
   -d      enable FUSE debug output; also implies foreground mode
   -s      single-threaded operation
   -o allow_root
           allow root to access this user-mounted FUSE filesystem
   -o allow_other
           allow all users to access this user-mounted FUSE filesystem

A normal user-mounted FUSE filesystem is visible only to the mounting user.
That means `sudo chown root:root mnt-narf/foo` can fail with `Permission
denied` before the NARF driver ever receives the chown request.  Mount with
`-o allow_root` when testing root-owned files:

   ./src/narf_fuse narf.img mnt-narf -o allow_root

Some distributions require this line in `/etc/fuse.conf` before non-root users
may use `allow_root` or `allow_other`:

   user_allow_other

After mounting with `allow_root`, this should work:

   sudo chown root:root mnt-narf/foo

The driver already serializes access with a mutex because the NARF core is not
thread-safe.  `-s` can still make debugging simpler.


8. Basic write test through FUSE
--------------------------------

After mounting:

   echo "created through FUSE" > mnt-narf/fuse-created.txt
   cat mnt-narf/fuse-created.txt
   truncate -s 4 mnt-narf/fuse-created.txt
   cat mnt-narf/fuse-created.txt
   mkdir -p mnt-narf/newdir
   mv mnt-narf/fuse-created.txt mnt-narf/newdir/renamed.txt
   ls -la mnt-narf/newdir

Then unmount:

   fusermount3 -u mnt-narf

You can reopen the image with narf_tester to inspect it:

   ./src/narf_tester narf.img <<'EOF'
   init
   ls /
   ls /newdir/
   cat newdir/renamed.txt
   quit
   EOF


9. Unix-ish metadata and user xattrs
------------------------------------

The FUSE layer stores compact, human-readable metadata in each NARF data
node's `m_metadata` area.  A typical form is:

   v1 uid=1000 gid=1000 mode=100644 mtime=1778706214 bs=4K

The FUSE driver understands these reserved fields:

   uid       owner user ID
   gid       owner group ID
   mode      full octal Unix mode, such as 100644 or 040755
   mtime     Unix epoch modification time, in seconds

NARF does not store atime.  FUSE reports atime and ctime as the same value as
mtime because that is good enough for this tiny filesystem and avoids a write on
every read.

Any other `key=value` token is exposed as a Linux `user.<key>` extended
attribute.  For example this internal metadata token:

   bs=4K

appears on Linux as:

   user.bs = 4K

Example:

   setfattr -n user.bs -v 4K mnt-narf/hello.txt
   getfattr -d mnt-narf/hello.txt

The FUSE driver does not expose the whole `m_metadata` string as an xattr.  It
only exposes non-reserved custom tokens.  Unknown valid `key=value` tokens are
ignored by stat/chmod/chown/utimens logic but preserved when those known fields
are rewritten.

If a new or changed `user.*` xattr would make the metadata string exceed
`NARF_METADATA_SIZE` bytes including the terminating NUL, `setxattr` fails with
`ENOSPC` and the old metadata is left unchanged.

Only simple whitespace-free values can be represented in the current text form.
This works well for values such as `4K`, `sha256.deadbeef`, or `tile-17`; it is
not an arbitrary binary xattr store.  That is a feature, not a bug wearing a
fake mustache.

Rename preserves custom xattrs automatically because the same NARF node is
renamed.  Copy preserves them only if the userspace copy tool asks for xattrs,
for example:

   cp -a source dest
   cp --preserve=xattr source dest
   rsync -X source dest


10. Troubleshooting
-------------------

`pkg-config fuse3 --cflags --libs` fails during build:

   Install `pkg-config` and `libfuse3-dev`.

`./src/narf_fuse: open existing: No such file or directory`:

   The image path is wrong.  Create the image with narf_tester first or pass the
   correct path.

`fusermount3: failed to open /dev/fuse`:

   FUSE is not available to this environment.  This is common in locked-down
   containers.  Try on a normal Linux host or run the container with FUSE device
   access and the needed privileges.

`mountpoint is not empty`:

   Use an empty directory as the mount point, or pass the appropriate FUSE option
   if your setup deliberately allows non-empty mount points.

`Transport endpoint is not connected`:

   The FUSE process probably crashed or was killed.  Clean up with:

      fusermount3 -uz mnt-narf

Then remount and reproduce with `-f -d` so the failure is visible.
