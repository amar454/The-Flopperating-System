BITS 32
SECTION .text

extern isr_dispatch
global isr_stub_table

isr_common:
    cld

    ; push general purpose registers
    pusha

    ; push segment registers
    push ds
    push es
    push fs
    push gs

    ; switch to kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; argument is pointer to int frame
    push esp
    call isr_dispatch

    ; pop argument
    add esp, 4

    ; restore segment registers
    pop gs
    pop fs
    pop es
    pop ds

    ; restore general purpose registers
    popa

    ; fix stack and discard error code
    add esp, 8

    ; return from interrupt
    ; this instruction restores ip, cs, eflags, etc
    iretd

; isr without error code
%macro ISR_NOERR 1
isr_stub_%1:
    push dword 0  ; fake error code
    push dword %1 ; int number
    jmp isr_common
%endmacro

; isr with error code
%macro ISR_ERR 1
isr_stub_%1:
    push dword %1 ; push int number
    jmp isr_common
%endmacro

; generate stubs for all interrupts
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19

; remaining interrupts do not push an error code
%assign i 20
%rep 236
ISR_NOERR i
%assign i i+1
%endrep

; read only table of isr stub addresses
; c code will index this to fill the idt entries
SECTION .rodata
align 4
isr_stub_table:
%assign i 0
%rep 256
    dd isr_stub_%+i ; addr of isr stub [i]
%assign i i+1
%endrep
