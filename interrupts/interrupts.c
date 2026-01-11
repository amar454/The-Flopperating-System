/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
#include "interrupts.h"
#include "../task/sched.h"
#include "../drivers/io/io.h"
#include "../drivers/vga/vgahandler.h"
#include "../drivers/keyboard/keyboard.h"
#include "../lib/logging.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../mem/vmm.h"
#include "../mem/pmm.h"
#include "../mem/paging.h"

#include "../mem/utils.h"
#include "../kernel/kernel.h"

uint32_t global_tick_count = 0;
idt_entry_t idt[IDT_SIZE];
idt_ptr_t idtp;

typedef struct int_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} int_frame_t;

void interrupts_stack_init() {
    uint32_t stack_top = (uint32_t) (interrupt_stack + ISR_STACK_SIZE);
    __asm__ volatile("mov %0, %%esp" ::"r"(stack_top));
}

static inline void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

#define PIT_FREQUENCY 100
#define IDT_FLAGS 0x8E

extern void* isr_stub_table[256];

typedef enum int_type {
    INT_TYPE_DIVIDE_BY_ZERO = 0,
    INT_TYPE_INVALID_OPCODE = 6,
    INT_TYPE_GPF = 13,
    INT_TYPE_PAGE_FAULT = 14,
    INT_TYPE_PIT = 32,
    INT_TYPE_KEYBOARD = 33,
    INT_TYPE_SYSCALL = 80,
} int_type_t;

extern int c_syscall_routine(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);

void isr_dispatch(int_frame_t* frame) {
    switch (frame->int_no) {
        case INT_TYPE_DIVIDE_BY_ZERO:
            log("isr0: divide by zero\n", RED);
            break;
        case INT_TYPE_INVALID_OPCODE:
            log("isr6: invalid opcode\n", RED);
            break;
        case INT_TYPE_GPF:
            log("isr13: GPF\n", RED);
            break;
        case INT_TYPE_PAGE_FAULT: {
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            log("isr14: page fault\n", RED);
            log_uint("CR2: ", cr2);
            log_uint("err code: ", frame->err_code);
            break;
        }
        case INT_TYPE_PIT:
            global_tick_count++;
            pic_eoi(0);
            return;
        case INT_TYPE_KEYBOARD:
            keyboard_handler();
            pic_eoi(1);
            return;
        case INT_TYPE_SYSCALL: {
            SYSCALL_ISR_DISPATCH(frame);
            return;
        }
        default:
            log_uint("Unhandled interrupt :( ", frame->int_no);
            break;
    }

    if (frame->int_no >= 32) {
        pic_eoi(frame->int_no - 32);
    }
    __asm__ volatile("hlt");
}

static inline void idt_set_entry(int n, uint32_t base) {
    // high 16 bits
    idt[n].base_low = base & 0xFFFF;
    // low 16 bits
    idt[n].base_high = (base >> 16) & 0xFFFF;

    // where the handler will execute
    // we want keep this in the kernel
    // so hard coding is ok
    idt[n].sel = KERNEL_CODE_SEGMENT;

    // reserved
    idt[n].always0 = 0;

    // type and privilege flags
    // this can be a constant for our purposes
    idt[n].flags = IDT_FLAGS;
}

// this does some shit idk
static void pic_init() {
    // enter init mode
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    // set interrupt vector offsets
    // master and slave
    outb(PIC1_DATA, PIC1_V_OFFSET);
    outb(PIC2_DATA, PIC2_V_OFFSET);

    // tell master that slave is at IRQ2
    outb(PIC1_DATA, PIC1_IRQ2);

    // tell slave its cascade ID
    outb(PIC2_DATA, PIC2_CSC_ID);

    // set pic to 8086 mode
    // this is for protected mode
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    // apply initial irq masks
    // interrupts disabled
    outb(PIC1_DATA, PIC1_MASK);
    outb(PIC2_DATA, PIC2_MASK);

    // programmable interrupt controller is done
    log("pic: init - ok\n", GREEN);
}

// timer initialization
static void pit_init() {
    // set divisor
    uint16_t divisor = PIT_BASE_FREQUENCY / PIT_FREQUENCY;

    // tell pit we are configuring channel 0
    // in square wave mode
    outb(PIT_COMMAND_PORT, PIT_COMMAND_MODE);

    // load divisor
    // high byte
    outb(PIT_CHANNEL0_PORT, divisor & PIT_DIVISOR_LSB_MASK);

    // low byte
    outb(PIT_CHANNEL0_PORT, (divisor >> PIT_DIVISOR_MSB_SHIFT) & PIT_DIVISOR_LSB_MASK);

    // timer is initialized
    log("pit: init - ok\n", GREEN);
}

static void idt_set_stubs() {
    // set each idt entry to its stub
    for (int i = 0; i < IDT_SIZE; i++) {
        idt_set_entry(i, (uint32_t) isr_stub_table[i]);
    }
}

static void idt_init() {
    // set limit to sizeof idt minus one
    idtp.limit = sizeof(idt) - 1;

    // set base to ref of idt
    idtp.base = (uint32_t) &idt;

    idt_set_stubs();

    // load the interrupt descriptor table
    __asm__ volatile("lidt %0" ::"m"(idtp));

    // unmask interrupts
    __asm__ volatile("sti");
    log("idt: init - ok\n", GREEN);
}

void interrupts_init() {
    interrupts_stack_init();
    pic_init();
    pit_init();
    idt_init();
    log("interrupts: init - ok.\n", GREEN);
}
