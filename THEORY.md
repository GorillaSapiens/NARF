NARF
====

Not A Real Filesystem

NARF is a small key/value storage layer for sector-addressed media.  It is
meant for embedded projects where FAT-style directory machinery is more than
you want, but where you still want named blobs, simple per-file attributes, and reasonably
robust updates.

NARF assumes the underlying device is made of 512-byte sectors with linear
sector addressing.  The platform-specific I/O layer supplies the functions in
`narf_io.h`:

```
bool     narf_io_open(void);
bool     narf_io_close(void);
uint32_t narf_io_sectors(void);
bool     narf_io_write(uint32_t sector, void *data);
bool     narf_io_read(uint32_t sector, void *data);
```

The core filesystem code does not know whether those sectors come from a disk
image, flash, RAM, an SD card, or an especially well-trained pigeon.

Public model
------------

The public API is key-based.  A caller does not hold raw filesystem node
addresses.  Operations identify entries by string key:

```
narf_alloc("foo", 0);
narf_append("foo", data, size);
narf_size("foo");
narf_free("foo");
```

A key is a NUL-terminated C string.  NUL is not allowed inside the key.  The
maximum key length is determined by the current on-disk node layout and is
intentionally large, roughly hundreds of bytes.  Keys are ordered with
`strcmp()`.  Path-like keys normally do not start with `/`; saving that byte in
every key is more useful than pretending NARF has a real root directory.

NARF does not have real directories.  It has keys.  Convenience functions such
as `narf_dirfirst()` and `narf_dirnext()` interpret keys as path-like strings by
using a caller-supplied separator, usually `/`.

On-disk layout
--------------

A mounted NARF filesystem starts at an origin sector.  For a whole-image
filesystem, the origin is usually 0.  For an MBR partition, the origin is the
partition start sector.

The first two sectors at the origin contain two copies of the root block.  A
root block contains:

* filesystem signature and `m_narf_version`, the current on-disk format version
* sector size and total sector count
* root references for the data tree and free tree
* allocation frontier values: `m_bottom` for payload growth and `m_top` for catalog-node growth
* a small root-resident stack of committed-safe spare catalog-node sectors
* a persisted LFSR seed for node-version generation
* `m_root_version`, the 32-bit root commit/version counter
* checksum

At mount time, NARF reads both root copies, validates checksums, and chooses the
newer valid root using modulo-2^32 version comparison.  Root writes alternate
between the two root sectors.  Version comparison uses wraparound ordering:
a root version is considered newer only when the forward distance from the
other version is nonzero and less than 2^31.

Nodes and references
--------------------

Internal node references are plain catalog-node sector numbers.  `END` is the
null reference.  A referenced sector must contain a valid node checksum and a
nonzero `m_node_version`, but tree links no longer carry an expected node
version beside the sector number.

There are three similarly named version fields with deliberately different jobs:

* `Root.m_narf_version` is the on-disk NARF format version.  Mount rejects root
  sectors whose format version does not match the compiled code.
* `Root.m_root_version` is the committed-root generation counter.  The two root
  copies are compared with this field to choose the newest valid root.
* `Node.m_root_version` records the transaction/root generation that wrote that
  catalog node.  During an open transaction, `write_node()` may rewrite a node in
  place only when this field equals the current transaction root version.
* `Node.m_node_version` is a nonzero per-node token used as a sanity/debugging
  guard for node contents.  It is no longer part of child references.

Each node contains:

* left and right child sector references
* AVL height
* one tree-specific payload union:
  * data nodes store payload start sector, payload length, byte size, and `m_metadata`
  * free nodes store free-extent start sector and length
* key string, used by data nodes and empty for free nodes
* `m_root_version`, the transaction/root generation that last wrote the node
* `m_node_version`, a nonzero per-node version token
* checksum

Node versions come from a 32-bit LFSR.  The current LFSR state is stored in the
root when a transaction commits, so remounting continues from the committed
state.  When a node sector is written into a fresh/COW sector, NARF rejects
version 0 and avoids the currently visible version in that sector.

Trees
-----

Current NARF catalog state is stored in two AVL trees:

1. **Data tree**

   Keyed by file key.  This is the authoritative tree for files/blobs.  Data
   nodes hold the file extent, byte size, and `m_metadata`.

2. **Free tree**

   Keyed by `(length, start, node-sector)`.  This tracks free payload extents by
   size, with deterministic tie breakers for equal-sized extents.  The tree is
   ordered for best-fit allocation, not for address lookup.  Defrag and
   coalescing therefore do address-based searches by walking the tree.

The core does not maintain a parent-pointer or directory index tree.  Directory
traversal is a prefix/separator scan over the data tree.

Transactions and power loss
---------------------------

NARF uses copy-on-write catalog records.  A catalog update writes new or
transaction-private node sectors first and writes the new root last.  If power
is lost before the root write, the previous root still points to the old
catalog-node sectors.  The abandoned node sectors are just unreachable junk.  If
power is lost after the root write, the new root points to the new catalog-node
sectors.

A catalog node written earlier in the same open transaction may be rewritten in
place because no committed root can point at that transaction-private sector
yet.  A committed node is not overwritten in place; writing a changed version
allocates another node sector, writes the replacement, and marks the old sector
as trash.  Trashed sectors are moved to the root-resident spare stack only after
the transaction commits successfully.

Catalog-node sectors therefore move through this lifecycle:

```
live
  A committed root can reach the node through the data tree, free tree, or other
  live catalog state.  The sector must not be overwritten in place.

transaction-private
  The node was newly allocated or COW-written during the current transaction.
  No committed root can reach this sector yet, so the transaction may
  rewrite the sector in place.

trash
  The transaction has replaced or deleted the node, but the old committed root
  may still need it if power fails before commit.  Trash is transaction-local and
  is not reusable storage.

spare
  The transaction committed successfully, so old trash sectors are no longer
  needed for rollback.  They are pushed onto the committed spare stack and may be
  reused by a later catalog-node allocation.
```

The important rule is that **trash is not spare**.  A sector becomes safe to
reuse only after the commit that made it unreachable from the previous committed
root.

The current implementation stores spare catalog-node sectors in a fixed-size
inline stack in the root block.  Transaction trash is tracked in a fixed-size RAM
array while a transaction is open.  If either structure overflows, extra dead
catalog sectors are safely leaked rather than reused too early.  There is not yet
an on-disk spare linked list, trash-page list, or catalog-node garbage collector.

Rollback on normal runtime failure restores the in-memory root state and clears
transaction-local dirty/trash tracking.  It does not need to erase abandoned
node sectors; committed roots decide what is reachable.

Payload data writes follow the same commit rule.  Overwrites and most resizes
allocate/write a fresh extent, copy any unchanged old data, write the new bytes,
then commit catalog state that points at the new extent.  The sector-aligned
append fast path may extend a payload into immediately following free/open
space, but the committed old metadata still describes the old shorter extent
until the root commit.  This keeps committed catalog state from pointing at
unwritten file data.

Allocation
----------

Data allocation first tries to satisfy the request from the free tree.  If no
suitable free extent exists, NARF allocates from the open space between the low
payload frontier and the high catalog-node frontier.

Catalog nodes are allocated from the high end of the filesystem one sector at a
time, or from the root-resident spare stack when a committed-safe node sector is
available.  File payload data grows upward from the low end.  The root tracks
the current bottom/top frontier values.  Normal allocations preserve a small
metadata reserve so that a full medium can still perform delete/cleanup-style
metadata updates.

Defragmentation
---------------

`narf_defrag()` is optional behind `NARF_USE_DEFRAG`.  It works as a loop of
small power-loss-safe transactions rather than doing one giant in-place rewrite.
The internal phases are implementation details, not public operations:

1. **carve** trims unused sector tails from over-allocated data extents and
   inserts the trimmed tail into the payload free tree.
2. **squish** scans free holes from low addresses upward.  For a hole, it finds
   the largest later data extent that fits, preferring the highest-addressed
   extent on ties, copies that payload into the hole, updates the data node, and
   frees the old location.
3. **widen** runs only after squish cannot make progress.  It finds a
   `[free][data]` pair whose adjacent data extent fits in the open scratch space
   between `root.m_bottom` and `root.m_top`.  It chooses the smallest movable
   adjacent data extent, using the lower hole as a tie breaker, copies that data
   to the current payload frontier, advances `root.m_bottom`, and reinserts the
   combined old hole plus old data location as a larger free extent.  The next
   squish pass can then bubble that data back down as `[data][free]`.
4. **tidy** is a legacy cleanup pass for zero-length parked catalog-node records
   at `root.m_top`.  Current free-extent insertion discards zero-length extents,
   so tidy normally has nothing to do on freshly-written images.

A successful carve, squish, widen, or tidy action commits its own transaction.
After a successful widen, `narf_defrag()` returns to squish so newly widened holes
can be used immediately.  The public command still remains just `narf_defrag()`;
callers do not choose individual phases.

Each defrag step writes payload data before committing catalog state.  The old
committed root therefore remains usable if power is lost before the step's new
root is written.

Free-extent insertion coalesces adjacent payload free extents.  When a merged
free extent reaches the current payload frontier, insertion lowers
`root.m_bottom`, turning that space back into the implicit free area between
payload data and catalog-node storage.

Directory-style traversal
-------------------------

`narf_dirfirst(dirname, sep)` and `narf_dirnext(dirname, sep, previous_key)`
provide path-like traversal over keys.  They do not create real directories.
They simply find keys that begin with `dirname` and do not contain another
separator below that level.

Example keys:

```
bar
biz
foo
groink/
groink/elephant
groink/zebra
zebra
```

`narf_dirfirst("", "/")` yields entries directly under the root, such as
`bar`, `biz`, `foo`, `groink/`, and `zebra`.  `narf_dirfirst("/", "/")` is
also accepted as a root-directory spelling, but the stored keys do not need a
leading separator.  Neither spelling yields `groink/elephant` or
`groink/zebra` at that level.

`narf_dirfirst("groink/", "/")` yields `groink/`, `groink/elephant`, and
`groink/zebra`.  `narf_dirfirst("/groink/", "/")` is accepted as the same
directory-prefix request without requiring the keys themselves to start with
`/`.

MBR support
-----------

When `NARF_MBR_UTILS` is enabled, NARF includes simple MBR helper functions:

* `narf_mbr()` writes a classic MBR sector.
* `narf_partition()` creates a NARF partition entry.
* `narf_format()` formats a NARF partition.
* `narf_findpart()` searches for a NARF partition.
* `narf_mount()` mounts a partition by number.

This is convenience code, not a general-purpose partition manager.

Limitations
-----------

NARF is intentionally small.  It is not thread-safe.  The core stores a 128-byte
metadata area per data node but does not enforce permissions, ownership, or
timestamps by itself; the FUSE front-end can interpret that area as compact
Unix-ish metadata plus simple `user.*` xattrs.  The core has no journaling API
exposed to callers and no real directories.  It assumes the media layer handles
bad blocks and wear leveling, which is a reasonable assumption for many modern
microSD cards but may not be true for raw flash.

NARF is still Not A Real Filesystem.  The name is doing legal work.
