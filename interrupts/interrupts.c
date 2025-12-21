#include "interrupts.h"
#include "../task/sched.h"
#include "../drivers/io/io.h"
#include "../drivers/vga/vgahandler.h"
#include "../lib/logging.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdbool.h>
extern void* isr_stub_table[256];
uint32_t global_tick_count = 0;
idt_entry_t idt[IDT_SIZE];
idt_ptr_t idtp;

typedef enum {
    IRQ_PIT = 0,
    IRQ_KEYBOARD = 1,
    IRQ_CASCADE = 2,
    IRQ_COM2 = 3,
    IRQ_COM1 = 4,
    IRQ_LPT2 = 5,
    IRQ_FLOPPY = 6,
    IRQ_LPT1 = 7,
    IRQ_CMOS = 8,
    IRQ_UNIMPLEMENTED0 = 9,
    IRQ_UNIMPLEMENTED1 = 10,
    IRQ_UNIMPLEMENTED2 = 11,
    IRQ_UNIMPLEMENTED3 = 12,
    IRQ_UNIMPLEMENTED4 = 13,
    IRQ_UNIMPLEMENTED5 = 14,
    IRQ_UNIMPLEMENTED6 = 15
} irq_num_t;

static inline void pic_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

typedef struct interrupt_frame {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t useresp;
    uint32_t ss;
} interrupt_frame_t;

void interrupts_common(interrupt_frame_t* f) {
    switch (f->int_no) {
        case 0:
            log("Divide by zero", RED);
            for (;;) {
                asm volatile("hlt");
            }

        case 6:
            log("Invalid opcode", RED);
            for (;;) {
                asm volatile("hlt");
            }

        case 13:
            log("General protection fault", RED);
            for (;;) {
                asm volatile("hlt");
            }

        case 14: {
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            log("Page fault", RED);
            log_address("CR2: ", cr2);
            log_uint("ERR: ", f->err_code);
            for (;;) {
                asm volatile("hlt");
            }
        }

        case 32:
            global_tick_count++;
            sched_tick();
            pic_eoi(0);
            break;

        case 33:
            pic_eoi(1);
            break;

        default:
            log_uint("Unhandled interrupt: ", f->int_no);
            break;
    }
}

void idt_set_entry(int n, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[n].base_low = base & 0xFFFF;
    idt[n].base_high = (base >> 16) & 0xFFFF;
    idt[n].sel = sel;
    idt[n].always0 = 0;
    idt[n].flags = flags;
}

static void pic_init() {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    outb(PIC1_DATA, PIC1_V_OFFSET);
    outb(PIC2_DATA, PIC2_V_OFFSET);

    outb(PIC1_DATA, PIC1_IRQ2);
    outb(PIC2_DATA, PIC2_CSC_ID);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, PIC1_MASK);
    outb(PIC2_DATA, PIC2_MASK);

    log("pic: init - ok\n", GREEN);
}

static void pit_init() {
    uint16_t divisor = PIT_BASE_FREQUENCY / PIT_FREQUENCY;

    outb(PIT_COMMAND_PORT, PIT_COMMAND_MODE);

    outb(PIT_CHANNEL0_PORT, divisor & PIT_DIVISOR_LSB_MASK);
    outb(PIT_CHANNEL0_PORT, (divisor >> PIT_DIVISOR_MSB_SHIFT) & PIT_DIVISOR_LSB_MASK);
    log("pit: init - ok\n", GREEN);
}

extern void syscall_routine();

void idt_init(void) {
    idtp.limit = sizeof(idt_entry_t) * 256 - 1;
    idtp.base = (uint32_t) &idt;

    for (int i = 0; i < 256; i++) {
        idt_set_entry(i, (uint32_t) isr_stub_table[i], KERNEL_CODE_SEGMENT, 0x8E);
    }

    __asm__ volatile("lidt (%0)" ::"r"(&idtp));
    __asm__ volatile("sti");

    log("idt: init - ok\n", GREEN);
}

static void disable_interrupts() {
    __asm__ volatile("cli");
}

static void enable_interrupts() {
    __asm__ volatile("sti");
}

void interrupts_init() {
    pic_init();
    pit_init();
    idt_init();
    log("interrupts: init - ok.\n", GREEN);
}
