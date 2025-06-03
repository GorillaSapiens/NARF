tester.exe: tester.c narf_io.c narf.c
	gcc -Wall -g tester.c narf_io.c narf.c -lz -o tester

bootloader.bin: bootloader.asm
	nasm -f bin bootloader.asm -o bootloader.bin

clean:
	rm -rf tester


# vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
