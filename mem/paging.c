/*

Copyright 2024, 2025 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

*/

#include <stdint.h>
#include "pmm.h"
#include "utils.h"
#include "vmm.h"
#include "paging.h"
#include "../apps/echo.h"
#include "../drivers/vga/vgahandler.h"
#include "../lib/logging.h"

static inline uintptr_t kvirt_to_phys(void* v) {
    uintptr_t va = (uintptr_t) v;
    if (KERNEL_VIRT_BASE == 0)
        return va;
    if (va >= KERNEL_VIRT_BASE)
        return va - KERNEL_VIRT_BASE;
    return va;
}

static inline void* kphys_to_virt(uintptr_t p) {
    if (KERNEL_VIRT_BASE == 0)
        return (void*) p;
    return (void*) (p + KERNEL_VIRT_BASE);
}

uint32_t* pg_dir = (uint32_t*) 0xFFFFF000;
uint32_t* pg_tbls = (uint32_t*) 0xFFC00000;
uint32_t* current_pg_dir = NULL;

#define KERNEL_STACK_PAGING_ADDR 0xFF000000

static inline uint32_t page_dir_index_from_va(uint32_t va) {
    return (va >> 22) & 0x3FF;
}

static inline uint32_t virtual_page_index(uint32_t va) {
    return (va >> 12) & 0x3FF;
}

void load_pd(uint32_t* pd) {
    __asm__ volatile("mov %0, %%cr3" ::"r"(pd));
}

static void enable_paging(uint8_t enable_wp, uint8_t enable_pse) {
    uint32_t cr0;
    uint32_t cr4;

    __asm__ volatile("mov %%cr0, %0\n"
                     "or %1, %0\n"
                     "mov %0, %%cr0"
                     : "=r"(cr0)
                     : "r"(enable_wp ? 0x80010000 : 0x80000000) // 0x80010000 -> PG + WP
    );

    if (enable_pse) {
        __asm__ volatile("mov %%cr0, %0\n"
                         "or %1, %0\n"
                         "mov %0, %%cr0"
                         : "=r"(cr0)
                         : "r"(enable_wp ? 0x80010000 : 0x80000000)
                         : "memory");
    }
}

static void zero_area(void* area) {
    flop_memset(area, 0, TABLE_BYTES);
}

uint32_t* pd;
uint32_t* pt0;
uint32_t* pt1022;

uintptr_t pt0_phys;
uintptr_t pt1022_phys;
uintptr_t pd_phys;
uintptr_t new_pt_phys;

static int paging_init_page_directory() {
    uintptr_t pd_phys = (uintptr_t) pmm_alloc_page();
    if (!pd_phys) {
        log("pmm_alloc_page failed for pd\n", RED);
        return -1;
    }
    uint32_t* pd = (uint32_t*) kphys_to_virt(pd_phys);
    zero_area(pd);
    return 0;
}

static int paging_alloc_and_zero_pt(uintptr_t* out_phys, uint32_t** out_virt, const char* name) {
    *out_phys = (uintptr_t) pmm_alloc_page();
    if (!*out_phys) {
        log("pmm_alloc_page failed for ", RED);
        log(name, RED);
        log("\n", RED);
        return -1;
    }

    *out_virt = (uint32_t*) kphys_to_virt(*out_phys);
    zero_area(*out_virt);
    return 0;
}

static void paging_fill_identity_mapping(uint32_t* pt) {
    for (uint32_t i = 0; i < PAGE_ENTRIES; ++i) {
        pt[i] = (uint32_t) ((((uintptr_t) i * TABLE_BYTES) & PAGE_MASK) | PAGE_PRESENT | PAGE_RW);
    }
}

static int paging_init_first_page_table() {
    uintptr_t pt0_phys = 0;
    uint32_t* pt0 = NULL;

    if (paging_alloc_and_zero_pt(&pt0_phys, &pt0, "pt0") < 0) {
        return -1;
    }
    paging_fill_identity_mapping(pt0);
    return 0;
}

static int paging_init_recursive_page_table_slot() {
    uintptr_t pt_phys = 0;
    uint32_t* pt_virt = NULL;

    if (paging_alloc_and_zero_pt(&pt_phys, &pt_virt, "pt1022") < 0) {
        return -1;
    }

    return 0;
}

static int paging_init_page_tables() {
    if (paging_init_first_page_table() < 0) {
        return -1;
    }

    if (paging_init_recursive_page_table_slot() < 0) {
        return -1;
    }

    return 0;
}

static int paging_init_recursive_mapping() {
    pd[1022] = (uint32_t) (pt1022_phys & PAGE_MASK) | PAGE_PRESENT | PAGE_RW;
    uint32_t* pt1022 = (uint32_t*) kphys_to_virt(pt1022_phys);
    zero_area(pt1022);
    pd[1023] = (uint32_t) (pd_phys & PAGE_MASK) | PAGE_PRESENT | PAGE_RW;
    pt1022[1023] = (uint32_t) (pd_phys & PAGE_MASK) | PAGE_PRESENT | PAGE_RW;
    return 0;
}

static int paging_init_paging_stack() {
    uint32_t pt_idx = page_dir_index_from_va(KERNEL_STACK_PAGING_ADDR);
    uintptr_t new_pt_phys = (uintptr_t) pmm_alloc_page();
    if (!new_pt_phys) {
        log("pmm_alloc_page failed for new_pt\n", RED);
        return -1;
    }
    pg_dir[pt_idx] = (uint32_t) (new_pt_phys & PAGE_MASK) | PAGE_PRESENT | PAGE_RW;
    uint32_t* new_pt_virt = &pg_tbls[pt_idx * PAGE_ENTRIES];
    zero_area(new_pt_virt);
    invlpg((void*) KERNEL_STACK_PAGING_ADDR);
    return 0;
}

void paging_init(void) {
    int page_directory_status = paging_init_page_directory();

    if (page_directory_status < 0) {
        log("paging_init_page_directory failed\n", RED);
        return;
    }

    int page_tables_status = paging_init_page_tables();

    if (page_tables_status < 0) {
        log("paging_init_page_tables failed\n", RED);
        return;
    }

    int paging_recusrive_pagemap_status = paging_init_recursive_mapping();

    if (paging_recusrive_pagemap_status < 0) {
        log("paging_init_recursive_mapping failed\n", RED);
        return;
    }

    current_pg_dir = pd;
    load_pd(pd);
    log("page directory loaded\n", YELLOW);
    enable_paging(0, 0);
    log("paging enabled\n", YELLOW);

    int paging_setup_stack_status = paging_init_paging_stack();

    if (paging_setup_stack_status < 0) {
        log("paging_init_paging_stack failed\n", RED);
        return;
    }

    current_pg_dir = (uint32_t*) pg_dir;

    if (page_directory_status < 0 || page_tables_status < 0 || paging_recusrive_pagemap_status < 0 ||
        paging_setup_stack_status < 0) {
        log("paging_init: Initialization failed\n", RED);
        __asm__ volatile("hlt");
        return;
    }

    log("paging: init - ok\n", YELLOW);
}
