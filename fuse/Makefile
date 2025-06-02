all: prep narf_fuse narf_mkfs

prep:
	cp ../src/narf.[ch] .

narf_fuse: narf_fuse.c
	gcc -Wall narf_fuse.c -o narf_fuse `pkg-config fuse3 --cflags --libs`

narf_mkfs: narf_mkfs.c
	gcc -Wall narf_mkfs.c -o narf_mkfs
