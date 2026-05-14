CC     := gcc
ERR    := -Wall -Wextra -Wpedantic -Werror
CFLAGS := -MMD -MP -MF $(ERR) -g

SRC := tester.c narf_io.c narf.c
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

tester: $(OBJ)
	$(CC) $(OBJ) -o $@

bootloader.bin: bootloader.asm
	nasm -f bin bootloader.asm -o bootloader.bin

clean:
	rm -rf tester

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

-include $(DEP)

# vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
