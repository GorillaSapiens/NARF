=== v3
on-disk format version is now 10 after removing the persisted node-version/LFSR fields and adding the generic node m_next field
transaction-private catalog nodes now form a RAM-headed rollback chain through m_next, allowing ordinary rollback to restore consumed spares without rebuilding the whole spare cache
mount and fast fsck now perform linear-time catalog bounds, key, payload, AVL, ordering, and count checks; fsck deep additionally checks duplicate references, cycles, overlaps, and complete allocation coverage
mount now requires the root's stored origin to match the requested origin,
rejects filesystems extending past the device or selected MBR partition, and
clears all mounted state when no root copy can be accepted
AVL height and balance calculations now propagate catalog read failures instead of treating unreadable children as empty subtrees
recursive FUSE directory moves now use prefix iterators that enumerate every descendant key
file-backed narf_io_write() implementations fsync each completed sector write so write ordering is durable without extending the platform I/O interface
fixed power-loss and failure-path catalog corruption
full FUSE support including mode, mtime, uid, gid, xattr
incremental defrag uses carve/squish/widen/tidy implementation phases internally
metadata spare reuse now uses a RAM-only spare list rebuilt from catalog-node reachability instead of a root inline spare stack
redundant successor retirement is removed, and a retired-list overflow rebuilds the complete RAM spare chain after commit
DEFRAG_DEBUG reports how far the lowest free hole has advanced from its
post-carve starting sector toward the perfect-compaction frontier
(2 + total file extent sectors)

=== v1
minimal functionality
