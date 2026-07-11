NARF
====

Not A Real Filesystem

NARF is aimed at storage for small embedded systems.
NARF is missing a LOT of features you might expect from a real filesystem.

NARF files aren't really even files.
they're more like blobs in a database.
there is a string key, and some data associated with the key.
lots of reads are expected.
but only the occasional write.

while there are some convenience functions that treat keys like file paths, there are no real directories.

it is not thread safe.

there is no access control.

the core has no built-in timestamp fields.

the FUSE front-end can stash mtime in the metadata blob when you want Unix-ish behavior.

because

NARF is Not A Real Filesystem.

(but there's a FUSE driver, so you can mount it in Linux...)
