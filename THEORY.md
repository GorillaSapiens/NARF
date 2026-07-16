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
* `m_root_version`, the 32-bit root commit/version counter
* checksum

At mount time, NARF reads both root copies, validates checksums, and chooses the
newer valid root using modulo-2^32 version comparison.  Root writes alternate
between the two root sectors.  Version comparison uses wraparound ordering:
a root version is considered newer only when the forward distance from the
other version is nonzero and less than 2^31.

A root is accepted only when its stored origin exactly matches the origin the
caller requested and its declared sector count fits in the available device
range.  `narf_mount()` additionally restricts that range to the selected MBR
partition.  Candidate roots are rejected before they can remain visible as a
mounted filesystem; when neither copy is usable, the core clears its root,
transaction, retired-sector, and spare-list state.

Nodes and references
--------------------

Internal node references are plain catalog-node sector numbers.  `END` is the
null reference.  A referenced sector must contain a valid node checksum; tree
links carry only the sector number.

There are three version fields with deliberately different jobs:

* `Root.m_narf_version` is the on-disk NARF format version.  Mount rejects root
  sectors whose format version does not match the compiled code.
* `Root.m_root_version` is the committed-root generation counter.  The two root
  copies are compared with this field to choose the newest valid root.
* `Node.m_root_version` records the transaction/root generation that wrote that
  catalog node.  During an open transaction, `write_node()` may rewrite a node in
  place only when this field equals the current transaction root version.

Each node contains:

* left and right child sector references
* AVL height
* one tree-specific payload union:
  * data nodes store payload start sector, payload length, byte size, and `m_metadata`
  * free nodes store free-extent start sector and length
* key string, used by data nodes and empty for free nodes
* `m_root_version`, the transaction/root generation that last wrote the node
* `m_next`, used only to link transaction-private nodes onto the RAM rollback chain
* checksum

For a committed live tree node, `m_next` is ignored.  In a node allocated by the
current transaction it links the RAM-headed transaction rollback chain.  Spare
nodes do not use `m_next`: because they are not AVL-tree members, they reuse
`m_left` as the previous/lower-sector spare link and `m_right` as the
next/higher-sector spare link.

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

AVL height recomputation and balance checks are fallible operations: every child
height requires a validated catalog-node read.  A failed read aborts the current
mutation and rolls the transaction back; it is never interpreted as an empty
subtree with height zero.

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
as transaction-local retired.  Retired sectors are not reusable until after the
transaction commits successfully.

Catalog-node sectors therefore move through this lifecycle:

```
live
  A committed root can reach the node through the data tree, free tree, or other
  live catalog state.  The sector must not be overwritten in place.

transaction-private
  The node was newly allocated or COW-written during the current transaction.
  No committed root can reach this sector yet, so the transaction may
  rewrite the sector in place.

retired
  The transaction has replaced or deleted the node, but the old committed root
  may still need it if power fails before commit.  Retired state is
  transaction-local and is not reusable storage.

spare
  The transaction committed successfully, so old retired sectors are no longer
  needed for rollback.  They may be linked onto the RAM-only spare list and
  reused by a later catalog-node allocation.
```

The important rule is that **retired is not spare**.  A sector becomes safe to
reuse only after the commit that made it unreachable from the previous committed
root.

The current implementation keeps both ends of the spare list in RAM only.  The
list is sorted by sector address and doubly linked through the spare nodes:
`m_left` points to the previous/lower spare and `m_right` points to the
next/higher spare.  `spare_head` therefore names the lowest spare and
`spare_tail` names the highest.  Catalog allocation removes nodes from the tail,
which preserves low spares and maximizes the chance that the catalog frontier can
contract upward.  Spare-list links are cache state, not durable filesystem truth.

On mount, NARF rebuilds the RAM spare list with a framed reachability bitmap.  The
512-byte `spare_work` buffer is treated as 4096 bits, representing one
4096-sector catalog frame.  For each frame, NARF clears the bitmap, walks the
committed data and free trees once each, and marks every live catalog node whose
sector falls inside the frame.  Unmarked sectors in the intersection of that
frame and `[root.m_top, root.m_total_sectors)` are unreachable.  Frames and
sectors are scanned from high to low.  One pending spare is delayed until the
next lower spare is known, allowing each rebuilt doubly linked record to be
written exactly once.

Each tree walk uses only `node_work0`: after reading a node, the left and right
child sectors are copied into local variables before recursion, so subsequent
recursive reads may overwrite the shared buffer.  `node_work1` constructs and
writes spare records while `spare_work` retains the frame bitmap.  General
allocator paths use the existing sector scratch instead of borrowing either node
work buffer, because `write_node()` may be holding its caller's node in `work0`
or `work1`.  The rebuild therefore uses fixed sector-buffer memory.  If the
catalog region has `C` sectors and the two live trees contain `L` nodes, the
blocked scan costs approximately `O(C + L * ceil(C / 4096))`, rather than
performing two complete tree searches for each of the `C` candidate sectors.  A
damaged or cyclic tree is stopped by the catalog-read validation and AVL-depth
bound; rebuild failure leaves the disposable spare cache uninitialized.

Whenever the lowest catalog sectors are unreachable, NARF returns them to the
virgin gap by advancing `root.m_top`.  For a normal transaction, the new frontier
is calculated from the sorted low spare prefix plus sectors retired by that
transaction, and is included in the new root commit.  Only after that root commit
succeeds are the disposable list links adjusted.  Mount-time reconstruction may
also raise the in-memory frontier; the next successful root commit persists it.
Thus committed catalog growth is reversible when all catalog sectors at the low
boundary become unused.

During a transaction, every allocated catalog sector is linked through its
`m_next` field onto a RAM-headed rollback chain.  Rewriting a transaction-private
node in place preserves that link.  Because normal allocation consumes the
highest spare first, consumed preexisting spares are a high suffix of the sorted
list.  The rollback chain presents that suffix in ascending order, so ordinary
software rollback can append it back to the tail in `O(k)` work for `k` consumed
spares.  Sectors obtained by lowering `m_top` are skipped and become virgin again
when the saved root frontier is restored.  If a rollback-chain node cannot be
read or rewritten, the disposable spare cache is invalidated and rebuilt later.

The rollback head is not persistent.  After power loss, the committed root still
determines the correct live set, but mount must rebuild the spare list from
reachability.  After a successful commit, the rollback head is simply discarded;
its stale `m_next` values in newly committed live nodes are ignored.

Transaction-retired sectors are tracked in a fixed-size RAM array while a
transaction is open.  After a successful commit, any retired sectors not reclaimed
by raising `m_top` are inserted into sector order in the spare list.  Locating an
insertion point can require a read-only list scan, but only the new spare and its
two immediate neighbors are rewritten.  If the retired array overflows, NARF
rebuilds the entire spare chain from the newly committed roots.  If spare
recycling or rebuilding fails, the RAM spare cache is discarded and retried by a
later transaction or mount.  There is no on-disk retired-page list or full
catalog-node garbage collector.

Rollback on normal runtime failure restores the in-memory root state, restores
consumed spares through the rollback chain, and clears transaction-local retired
tracking.  It does not need to erase virgin abandoned node sectors; the restored
`m_top` places them outside the catalog region again.

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
time, or from the highest-address entry in the RAM spare list when a
committed-safe node sector is available.  File payload data grows upward from
the low end.  The root tracks the current bottom/top frontier values.  When a
contiguous low-address prefix of the catalog region becomes spare, `m_top` moves
up and returns those sectors to the open gap.  Normal allocations preserve a
small metadata reserve so that a full medium can still perform
delete/cleanup-style metadata updates.

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

`narf_prefixfirst(prefix)` and `narf_prefixnext(prefix, previous_key)` scan
all keys beginning with an exact key prefix, including every nested descendant.
Unlike the directory iterators, they do not stop at the next separator.  The
`previous_key` argument is a lexicographic cursor and does not need to still
exist, so callers may rename or delete the returned key before requesting the
next match.  The FUSE directory-rename loop uses this property to move an
entire subtree incrementally.

Consistency checking
--------------------

Mount validation and ordinary `narf_fsck()` perform checks that require only
linear tree walks and bounded recursion.  They verify catalog-sector bounds,
child references, key termination, payload/free-extent ranges, data/free tree
ordering, stored AVL heights and balance, direct cycles or duplicate sibling
links, and the root data-node count.

`narf_fsck_deep()` adds the checks that need whole-filesystem accounting.  It
marks catalog sectors to detect repeated references, cross-tree sharing, cycles,
and overlap with the spare chain.  It also builds payload coverage information
to detect overlapping file/free extents, leaked payload sectors, and any gap or
double allocation in the payload region.  The deep catalog map uses one byte
per possible catalog-sector address while the check runs.

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
