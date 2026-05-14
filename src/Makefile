CC     := gcc
ERR    := -Wall -Wextra -Wpedantic -Werror
CFLAGS := -MMD -MP -MF $(ERR) -g

TSRC := tester.c narf_io.c narf.c
TOBJ := $(TSRC:.c=.o)
TDEP := $(TOBJ:.o=.d)

FSRC := narf_fuse.c narf.c
FOBJ := $(FSRC:.c=.o)
FDEP := $(FOBJ:.o=.d)

MSRC := narf_mkfs.c narf.c
MOBJ := $(MSRC:.c=.o)
MDEP := $(MOBJ:.o=.d)

all: narf_tester narf_fuse narf_mkfs

narf_tester: $(TOBJ)
	$(CC) $(TOBJ) -o $@

narf_fuse: $(FOBJ)
	$(CC) $(FOBJ) -o $@ `pkg-config fuse3 --cflags --libs`

narf_mkfs: $(MOBJ)
	$(CC) $(MOBJ) -o $@

bootloader.bin: bootloader.asm
	nasm -f bin bootloader.asm -o bootloader.bin

clean:
	rm -rf tester

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

-include $(DEP)

# vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
