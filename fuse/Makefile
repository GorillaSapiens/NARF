CC     := gcc
ERR    := -Wall -Wextra -Wpedantic -Werror
CFLAGS := -MMD -MP -MF $(ERR) -g

FSRC := narf_fuse.c narf.c
FOBJ := $(FSRC:.c=.o)
FDEP := $(FOBJ:.o=.d)

MSRC := narf_mkfs.c narf.c
MOBJ := $(MSRC:.c=.o)
MDEP := $(MOBJ:.o=.d)

all: prep narf_fuse narf_mkfs

prep: # we always copy, something may have changed...
	cp ../src/narf.[ch] .
	cp ../src/narf_mbr.h .

narf_fuse: $(FOBJ)
	$(CC) $(FOBJ) -o $@ `pkg-config fuse3 --cflags --libs`

narf_mkfs: $(MOBJ)
	$(CC) $(MOBJ) -o $@

clean:
	rm -f narf_fuse narf_mkfs

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

-include $(DEP)

# vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
