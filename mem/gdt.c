/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
#include <stdint.h>
#include "gdt.h"
#include "../task/tss.h"
#include "../lib/logging.h"

static uint64_t our_gdt[] = {
    0x0000000000000000ULL,
    0x00CF9A000000FFFFULL,
    0x00CF92000000FFFFULL,
    0x00CFFA000000FFFFULL,
    0x00CFF2000000FFFFULL,
    0x0000000000000000ULL // tss (garbage for now)
};

static const gdtr_t gdt_reg = {sizeof(our_gdt) - 1, (uint32_t) our_gdt};

// only used for tss, rest is done statically
void gdt_set_gate(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    uint64_t desc = 0;

    desc |= (limit & 0xFFFFULL);
    desc |= ((base & 0xFFFFFFULL) << 16);
    desc |= ((uint64_t) access << 40);
    desc |= ((uint64_t) (limit >> 16) & 0x0F) << 48;
    desc |= ((uint64_t) (gran & 0xF0) << 48);
    desc |= ((uint64_t) (base >> 24) & 0xFF) << 56;

    our_gdt[idx] = desc;
}

void flush_gdt() {
    __asm__ volatile("lgdt %0\n"
                     "mov $0x10, %%ax\n"
                     "mov %%ax, %%ds\n"
                     "mov %%ax, %%es\n"
                     "mov %%ax, %%fs\n"
                     "mov %%ax, %%gs\n"
                     "mov %%ax, %%ss\n"
                     "jmp $0x08, $.flush\n"
                     ".flush:\n"
                     :
                     : "m"(gdt_reg)
                     : "eax");
}

void gdt_init() {
    flush_gdt();
    tss_init(3, 0x10, 0x0);

    log("gdt: init - ok\n", GREEN);
}
