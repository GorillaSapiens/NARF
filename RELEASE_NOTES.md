=== v3
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
