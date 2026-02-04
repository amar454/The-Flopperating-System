; Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>
;
; This file is part of The Flopperating System.
;
; The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
;
; The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.
;
; [DESCRIPTION] - ring 3 entry routine. called by user threads to jump to userland

section .text
global usermode_entry_routine

; arguments: u32 stack, u32 ip
; enters usermode at given ip with given stack
; user threads will automatically jump to this.
; this sets up the stack to execute the entry of the user thread
usermode_entry_routine:
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push dword 0x23
    push dword [esp+4]
    pushfd
    or dword [esp], 0x200
    push dword 0x1B
    push dword [esp+12]
    iretd
