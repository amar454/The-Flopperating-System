/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later veregion_startion.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

*/
#include "tss.h"
#include "../mem/gdt.h"
#include "../mem/utils.h"
#include "../lib/logging.h"

extern void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);

static tss_entry_t tss_entry;

void tss_init(uint32_t idx, uint32_t kss, uint32_t kesp) {
    uint32_t base = (uint32_t) &tss_entry;
    uint32_t limit = base + sizeof(tss_entry_t);

    gdt_set_gate(idx, base, limit, 0x89, 0x00);

    flop_memset(&tss_entry, 0, sizeof(tss_entry));

    tss_entry.ss0 = kss;
    tss_entry.esp0 = kesp;
    tss_entry.cs = 0x0b;
    tss_entry.ss = 0x13;
    tss_entry.ds = 0x13;
    tss_entry.es = 0x13;
    tss_entry.fs = 0x13;
    tss_entry.gs = 0x13;
    tss_entry.iomap_base = sizeof(tss_entry_t);

    __asm__ volatile("ltr %%ax" : : "a"((idx * 8) | 0x0));

    log("tss: init - ok\n", GREEN);
}

void tss_set_kernel_stack(uint32_t stack) {
    tss_entry.esp0 = stack;
}
