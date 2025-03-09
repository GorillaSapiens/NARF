NARF
====

Not A Real Filesystem

NARF is aimed at storage for small embedded systems.
NARF is missing a LOT of features you might expect from a real filesystem.

NARF files are more like blobs in a database.
There is a string key, and some data associated with the key.
Lots of reads are expected.
but only the occasional write.
deletes rarely happen.

NARF is not a real filesystem.
