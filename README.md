NARF
====

Not A Real Filesystem

NARF is aimed at storage for small embedded systems.
NARF is missing a LOT of features you might expect from a real filesystem.

NARF files aren't really even files.
they're are more like blobs in a database.
there is a string key, and some data associated with the key.
lots of reads are expected.
but only the occasional write.
deletes rarely happen, and they're very expensive.

while there are some convenience functions that treat keys like file pahts, there are no real directories.

there is no access control.

there are no timestamps.

there is no redundancy.

there is no wear leveling.

there is no defragmentation.

we don't need them

because

NARF is Not A Real Filesystem.
