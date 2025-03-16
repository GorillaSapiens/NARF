org 0x7c00                  ; Set origin to 0x7c00

; compile with:
; nasm -f bin bootloader.asm -o bootloader.bin

; emulate with:
; qemu-system-x86_64 -drive format=raw,file=bootloader.bin

bits 16                     ; 16-bit real mode

start:
    jmp main                ; Jump to main code

; Bootloader code starts here

main:
    mov ax, 0x07C0          ; Set up data segment
    mov ds, ax
    mov es, ax

    mov si, message         ; Print a message
    call print_string

    jmp $                   ; Infinite loop

; Print a null-terminated string
print_string:
    lodsb                   ; Load the next character
    or al, al               ; Check for null terminator
    jz end_print_string     ; If null, end of string

    call print_char         ; Print the character
    jmp print_string        ; Repeat for the next character

end_print_string:
    ret

; Print a character in AL
print_char:
    mov ah, 0x0E            ; BIOS teletype function
    int 0x10                ; Call BIOS interrupt

    ret

; Data section
message db "NARF! not bootable.", 0x0D, 0x0A, 0

times 510-($-$$) db 0    ; Pad the bootloader to 510 bytes
dw 0xAA55                ; Boot signature at 511-512 bytes

; vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
