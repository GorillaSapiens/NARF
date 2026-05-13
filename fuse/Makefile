ERR=-Wall -Wextra -Wpedantic -Werror

all: prep narf_fuse narf_mkfs

prep:
	cp ../src/narf.[ch] .

narf_fuse: narf_fuse.c narf.c narf.h
	gcc -g $(ERR) narf_fuse.c narf.c -o narf_fuse `pkg-config fuse3 --cflags --libs`

narf_mkfs: narf_mkfs.c narf.c narf.h
	gcc -g $(ERR) narf_mkfs.c narf.c -o narf_mkfs

clean:
	rm -f narf_fuse narf_mkfs
