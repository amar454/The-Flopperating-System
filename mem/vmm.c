/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
#include <stdint.h>
#include "pmm.h"
#include "vmm.h"
#include "alloc.h"
#include "paging.h"
#include "utils.h"
#include "../lib/logging.h"

extern uint32_t* pg_dir;
extern uint32_t* pg_tbls;
extern uint32_t* current_pg_dir;
vmm_region_t kernel_region;
static vmm_region_t* current_region = NULL;

static inline uint32_t pd_index(uintptr_t va) {
    return (va >> 22) & 0x3FF;
}

static inline uint32_t pt_index(uintptr_t va) {
    return (va >> 12) & 0x3FF;
}

static inline uint32_t page_offset(uintptr_t va) {
    return va & 0xFFF;
}

#define RECURSIVE_ADDR 0xFFC00000
#define RECURSIVE_PT(pdi) ((uint32_t*) (RECURSIVE_ADDR + (pdi) * PAGE_SIZE))

// allocate a virtual address
uintptr_t vmm_alloc(vmm_region_t* region, size_t pages, uint32_t flags) {
    if (!region || pages == 0) {
        log("vmm_alloc: invalid region or zero pages\n", RED);
        return (uintptr_t) (-1);
    }

    uintptr_t va = region->next_free_va ? region->next_free_va : region->base_va;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = (uintptr_t) pmm_alloc_page();
        if (!pa) {
            log_uint("vmm_alloc: pmm_alloc_page failed at page index: ", i);
            log_address("vmm_alloc: unmapping allocated range starting virtual address: ", va);
            vmm_unmap_range(region, va, i);
            return (uintptr_t) (-1);
        }
        vmm_map(region, va + i * PAGE_SIZE, pa, flags);
    }

    region->next_free_va = va + pages * PAGE_SIZE;

    return va;
}

// free a virtual address
void vmm_free(vmm_region_t* region, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(region, va + i * PAGE_SIZE);
        if (pa) {
            pmm_free_page((void*) pa);
        }
        vmm_unmap(region, va + i * PAGE_SIZE);
    }
}

// map a page to a virtual address
int vmm_map(vmm_region_t* region, uintptr_t va, uintptr_t pa, uint32_t flags) {
    uint32_t pdi = pd_index(va);
    uint32_t pti = pt_index(va);
    
    // allocate a new pt if needed
    if (!(region->pg_dir[pdi] & PAGE_PRESENT)) {
        uintptr_t pt_phys = (uintptr_t) pmm_alloc_page();
        if (!pt_phys) {
            return -1;
        }
        region->pg_dir[pdi] = (pt_phys & PAGE_MASK) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        flop_memset(RECURSIVE_PT(pdi), 0, PAGE_SIZE);
    }
    
    uint32_t* pt = RECURSIVE_PT(pdi);
    pt[pti] = (pa & PAGE_MASK) | flags | PAGE_PRESENT;
    invlpg((void*) va);
    return 0;
}

// unmap a page from a virtual address
int vmm_unmap(vmm_region_t* region, uintptr_t va) {
    uint32_t pdi = pd_index(va);
    uint32_t pti = pt_index(va);
    if (!(region->pg_dir[pdi] & PAGE_PRESENT)) {
        return -1;
    }
    uint32_t* pt = RECURSIVE_PT(pdi);
    pt[pti] = 0;
    invlpg((void*) va);
    return 0;
}

// resolve a virtual address to a physical address
uintptr_t vmm_resolve(vmm_region_t* region, uintptr_t va) {
    uint32_t pdi = pd_index(va);
    uint32_t pti = pt_index(va);
    if (!(region->pg_dir[pdi] & PAGE_PRESENT)) {
        return 0;
    }
    uint32_t* pt = RECURSIVE_PT(pdi);
    if (!(pt[pti] & PAGE_PRESENT)) {
        return 0;
    }
    return (pt[pti] & PAGE_MASK) | (va & ~PAGE_MASK);
}

static spinlock_t region_list_lock = SPINLOCK_INIT;
static vmm_region_t* region_list = NULL;

void vmm_region_insert(vmm_region_t* region) {
    bool r = spinlock(&region_list_lock);
    region->next = region_list;
    region_list = region;
    spinlock_unlock(&region_list_lock, r);
}

void vmm_region_remove(vmm_region_t* region) {
    bool r = spinlock(&region_list_lock);
    vmm_region_t** iter = &region_list;
    while (*iter) {
        if (*iter == region) {
            *iter = region->next;
            break;
        }
        iter = &(*iter)->next;
    }
    spinlock_unlock(&region_list_lock, r);
}

// create a new region descriptor
vmm_region_t* vmm_region_create(size_t initial_pages, uint32_t flags, uintptr_t* out_va) {
    uintptr_t dir_phys = (uintptr_t) pmm_alloc_page();
    if (!dir_phys) {
        log("vmm_region_create: pmm_alloc_page failed\n", RED);
        return NULL;
    }

    uint32_t* dir = (uint32_t*) dir_phys;
    flop_memset(dir, 0, PAGE_SIZE);

    dir[RECURSIVE_PDE] = (dir_phys & PAGE_MASK) | PAGE_PRESENT | PAGE_RW | PAGE_USER;

    vmm_region_t* region = (vmm_region_t*) kmalloc(sizeof(vmm_region_t));
    if (!region) {
        log("vmm_region_create: kmalloc failed\n", RED);
        pmm_free_page((void*) dir_phys);
        return NULL;
    }

    region->pg_dir = dir;
    region->next = NULL;
    region->base_va = USER_SPACE_START;
    region->next_free_va = region->base_va;

    vmm_region_insert(region);

    if ((initial_pages > 0) && out_va) {
        uintptr_t va = vmm_alloc(region, initial_pages, flags);
        if (va == (uintptr_t) (-1)) {
            vmm_region_remove(region);
            kfree(region, sizeof(vmm_region_t));
            pmm_free_page((void*) dir_phys);
            return NULL;
        } else {
            *out_va = va;
        }
    }

    return region;
}

// destroy a region descriptor
void vmm_region_destroy(vmm_region_t* region) {
    if (!region) {
        return;
    }
    vmm_region_remove(region);
    pmm_free_page((void*) region->pg_dir);
    kfree(region, sizeof(vmm_region_t));
}

// switch to a region descriptors page directory
void vmm_switch(vmm_region_t* region) {
    if (!region) {
        return;
    }
    current_region = region;
    current_pg_dir = region->pg_dir;
    load_pd(region->pg_dir);
}

void vmm_init() {
    kernel_region.pg_dir = pg_dir;
    kernel_region.next = 0;
    current_pg_dir = pg_dir;
    pg_dir[RECURSIVE_PDE] = ((uintptr_t) pg_dir & PAGE_MASK) | PAGE_PRESENT | PAGE_RW;
    vmm_region_insert(&kernel_region);
    log("vmm: init - ok\n", GREEN);
}

vmm_region_t* vmm_get_current() {
    return current_region;
}

uint32_t* vmm_new_copied_pgdir() {
    uintptr_t new_dir_phys = (uintptr_t) pmm_alloc_page();
    if (!new_dir_phys) {
        return 0;
    }
    uint32_t* new_dir = (uint32_t*) new_dir_phys;
    flop_memset(new_dir, 0, PAGE_SIZE);
    return new_dir;
}

int vmm_copy_frames(uint32_t* src_pt, uint32_t* dst_pt) {
    for (int pti = 0; pti < PAGE_ENTRIES; pti++) {
        if (!(src_pt[pti] & PAGE_PRESENT)) {
            continue;
        }

        uintptr_t new_page = (uintptr_t) pmm_alloc_page();
        if (!new_page) {
            return -1;
        }

        flop_memcpy((void*) new_page, (void*) (src_pt[pti] & PAGE_MASK), PAGE_SIZE);
        dst_pt[pti] = (new_page & PAGE_MASK) | (src_pt[pti] & ~PAGE_MASK);
    }
    return 0;
}

int vmm_iterate_and_copy_page_tables(vmm_region_t* src, vmm_region_t* dst) {
    for (int pdi = 0; pdi < 1024; pdi++) {
        if (!(src->pg_dir[pdi] & PAGE_PRESENT)) {
            continue;
        }

        uintptr_t pt_phys = (uintptr_t) pmm_alloc_page();
        if (!pt_phys) {
            return -1;
        }

        uint32_t* src_pt = &pg_tbls[pdi * PAGE_ENTRIES];
        uint32_t* dst_pt = (uint32_t*) pt_phys;

        flop_memset(dst_pt, 0, PAGE_SIZE);

        if (vmm_copy_frames(src_pt, dst_pt) < 0) {
            pmm_free_page((void*) pt_phys);
            return -1;
        }

        dst->pg_dir[pdi] = (pt_phys & PAGE_MASK) | (src->pg_dir[pdi] & ~PAGE_MASK);
    }
    return 0;
}

vmm_region_t* vmm_copy_pagemap(vmm_region_t* src) {
    uint32_t* new_dir = vmm_new_copied_pgdir();
    if (!new_dir) {
        // yeah if this fails, fuck.
        return 0;
    }

    vmm_region_t* dst = (vmm_region_t*) kmalloc(sizeof(vmm_region_t));
    if (!dst) {
        kfree(dst, sizeof(vmm_region_t));
        return 0;
    }

    dst->pg_dir = new_dir;
    dst->next = 0;
    dst->base_va = src->base_va;
    dst->next_free_va = src->next_free_va;

    if (vmm_iterate_and_copy_page_tables(src, dst) < 0) {
        vmm_region_destroy(dst);
        return 0;
    }

    new_dir[RECURSIVE_PDE] = ((uintptr_t) new_dir & PAGE_MASK) | PAGE_PRESENT | PAGE_RW;

    vmm_region_insert(dst);
    return dst;
}

void vmm_free_physical_frames(uint32_t* pt) {
    for (int pti = 0; pti < PAGE_ENTRIES; pti++) {
        if (pt[pti] & PAGE_PRESENT) {
            pmm_free_page((void*) (pt[pti] & PAGE_MASK));
        }
    }
}

void vmm_iterate_through_page_tables(vmm_region_t* region) {
    for (int pdi = 0; pdi < 1024; pdi++) {
        if (!(region->pg_dir[pdi] & PAGE_PRESENT)) {
            continue;
        } else {
            uint32_t* pt = &pg_tbls[pdi * PAGE_ENTRIES];

            vmm_free_physical_frames(pt);

            uintptr_t pt_phys = region->pg_dir[pdi] & PAGE_MASK;
            pmm_free_page((void*) pt_phys);
        }
    }
}

void vmm_nuke_pagemap(vmm_region_t* region) {
    vmm_iterate_through_page_tables(region);

    uintptr_t dir_phys = (uintptr_t) region->pg_dir;
    pmm_free_page((void*) dir_phys);

    vmm_region_remove(region);
    kfree(region, sizeof(vmm_region_t));
}

vmm_region_t* vmm_find_region(uintptr_t va) {
    vmm_region_t* iter = region_list;
    while (iter) {
        if (vmm_resolve(iter, va)) {
            return iter;
        } else {
            iter = iter->next;
        }
    }
    return 0;
}

size_t vmm_count_regions() {
    size_t n = 0;
    vmm_region_t* iter = region_list;
    while (iter) {
        n++;
        iter = iter->next;
    }
    return n;
}

// map a range of virtual addresses to physical addresses
int vmm_map_range(vmm_region_t* region, uintptr_t va, uintptr_t pa, size_t pages, uint32_t flags) {
    for (size_t i = 0; i < pages; i++) {
        if (vmm_map(region, va + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags) < 0) {
            return -1;
        }
    }
    return 0;
}

// unmap a range of virtual addresses from physical addresses
int vmm_unmap_range(vmm_region_t* region, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        if (vmm_unmap(region, va + i * PAGE_SIZE) < 0) {
            return -1;
        }
    }
    return 0;
}

// protect a memory region with flags
int vmm_protect(vmm_region_t* region, uintptr_t va, uint32_t flags) {
    uint32_t pdi = pd_index(va);
    uint32_t pti = pt_index(va);
    if (!(region->pg_dir[pdi] & PAGE_PRESENT)) {
        return -1;
    }
    uint32_t* pt = &pg_tbls[pdi * PAGE_ENTRIES];
    if (!(pt[pti] & PAGE_PRESENT)) {
        return -1;
    }
    pt[pti] = (pt[pti] & PAGE_MASK) | flags | PAGE_PRESENT;
    invlpg((void*) va);
    return 0;
}

// get pt of va in region
uint32_t* vmm_get_pt(vmm_region_t* region, uintptr_t va) {
    uint32_t pdi = pd_index(va);
    if (!(region->pg_dir[pdi] & PAGE_PRESENT)) {
        return 0;
    }
    return &pg_tbls[pdi * PAGE_ENTRIES];
}

uint32_t vmm_get_pde(vmm_region_t* region, uintptr_t va) {
    return region->pg_dir[pd_index(va)];
}

// find a virtual address range that is free in the given region
uintptr_t vmm_find_free_range(vmm_region_t* region, size_t pages) {
    size_t run = 0;
    uintptr_t start = 0;
    for (uintptr_t va = 0; va < 0xFFFFFFFF; va += PAGE_SIZE) {
        uint32_t* pt = vmm_get_pt(region, va);
        uint32_t pde = vmm_get_pde(region, va);
        int used = 0;
        if (pde & PAGE_PRESENT) {
            if (pt && (pt[pt_index(va)] & PAGE_PRESENT)) {
                used = 1;
            }
        }
        if (!used) {
            if (run == 0) {
                start = va;
            }
            run++;
            if (run >= pages) {
                return start;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

int vmm_map_shared(
    vmm_region_t* a, vmm_region_t* b, uintptr_t va_a, uintptr_t va_b, uintptr_t pa, size_t pages, uint32_t flags) {
    for (size_t i = 0; i < pages; i++) {
        if (vmm_map(a, va_a + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags) < 0) {
            return -1;
        }
        if (vmm_map(b, va_b + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags) < 0) {
            return -1;
        }
    }
    return 0;
}

int vmm_identity_map(vmm_region_t* region, uintptr_t base, size_t pages, uint32_t flags) {
    return vmm_map_range(region, base, base, pages, flags);
}

int vmm_is_mapped(vmm_region_t* region, uintptr_t va) {
    return vmm_resolve(region, va) != 0;
}

size_t vmm_count_mapped(vmm_region_t* region) {
    size_t n = 0;
    for (uintptr_t va = 0; va < KERNEL_VIRT_BASE; va += PAGE_SIZE) {
        if (vmm_is_mapped(region, va)) {
            n++;
        }
    }
    return n;
}

int vmm_map_direct(vmm_region_t* region, uintptr_t phys, size_t pages, uint32_t flags) {
    for (size_t i = 0; i < pages; i++) {
        if (vmm_map(region, phys + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags) < 0) {
            return -1;
        }
    }
    return 0;
}

uintptr_t vmm_map_anonymous(vmm_region_t* region, size_t pages, uint32_t flags) {
    uintptr_t va = vmm_find_free_range(region, pages);
    if (!va) {
        return 0;
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = (uintptr_t) pmm_alloc_page();
        if (!pa) {
            for (size_t j = 0; j < i; j++) {
                vmm_unmap(region, va + j * PAGE_SIZE);
            }
            return 0;
        }
        if (vmm_map(region, va + i * PAGE_SIZE, pa, flags) < 0) {
            for (size_t j = 0; j <= i; j++) {
                vmm_unmap(region, va + j * PAGE_SIZE);
            }
            pmm_free_page((void*) pa);
            return 0;
        }
    }

    return va;
}

int vmm_protect_range(vmm_region_t* region, uintptr_t va, size_t pages, uint32_t flags) {
    for (size_t i = 0; i < pages; i++) {
        if (vmm_protect(region, va + i * PAGE_SIZE, flags) < 0) {
            log("vmm_protect_range: failed to protect page\n", RED);
            return -1;
        }
    }
    return 0;
}

uintptr_t vmm_alloc_stack(vmm_region_t* region, size_t pages, uint32_t flags) {
    if (!region || pages == 0) {
        return 0;
    }

    size_t total_pages = pages + 2;

    uintptr_t va_base = vmm_find_free_range(region, total_pages);
    if (!va_base) {
        log("vmm_alloc_stack: no free virtual range found\n", RED);
        return 0;
    }

    uintptr_t stack_start = va_base + PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = (uintptr_t) pmm_alloc_page();
        if (!pa) {
            vmm_unmap_range(region, stack_start, i);
            return 0;
        }
        vmm_map(region, stack_start + i * PAGE_SIZE, pa, flags);
    }

    if (region->next_free_va < va_base + total_pages * PAGE_SIZE) {
        region->next_free_va = va_base + total_pages * PAGE_SIZE;
    }

    return stack_start;
}

int vmm_map_scatter(vmm_region_t* region, uintptr_t va, uintptr_t* phys_pages, size_t pages, uint32_t flags) {
    if (!region || !phys_pages) {
        return -1;
    }

    if (vmm_is_range_mapped(region, va, pages)) {
        log("vmm_map_scatter: virtual range already mapped\n", RED);
        return -1;
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = phys_pages[i];
        if (vmm_map(region, va + i * PAGE_SIZE, pa, flags) < 0) {
            vmm_unmap_range(region, va, i);
            return -1;
        }
    }
    return 0;
}

int vmm_is_range_mapped(vmm_region_t* region, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        if (!vmm_resolve(region, va + i * PAGE_SIZE)) {
            return 0;
        }
    }
    return 1;
}

uint32_t vmm_get_flags(vmm_region_t* region, uintptr_t va) {
    uint32_t* pt = vmm_get_pt(region, va);
    if (!pt) {
        return 0;
    }

    uint32_t pti = pt_index(va);
    uint32_t entry = pt[pti];

    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }

    return entry & 0xFFF;
}

void vmm_dump_map(vmm_region_t* region) {
    uintptr_t run_start = 0;
    int in_run = 0;

    for (uintptr_t va = 0; va < 0xFFFFF000; va += PAGE_SIZE) {
        int mapped = vmm_is_mapped(region, va);

        if (mapped && !in_run) {
            run_start = va;
            in_run = 1;
        } else if (!mapped && in_run) {
            log_address("Start: ", run_start);
            log_address("End:   ", va);
            log_uint("size in pages:", (va - run_start) / PAGE_SIZE);
            in_run = 0;
        }
    }
    if (in_run) {
        log_address("Start: ", run_start);
        log("End:   Top of Memory\n", WHITE);
    }
}

uintptr_t vmm_find_free_range_aligned(vmm_region_t* region, size_t pages, size_t alignment) {
    size_t run = 0;
    uintptr_t start = 0;
    uintptr_t va = 0;

    if (region->next_free_va != 0) {
        va = region->next_free_va;
        if (va % alignment != 0) {
            va = (va + alignment - 1) & ~(alignment - 1);
        }
    } else {
        va = region->base_va;
    }

    for (; va < 0xFFFFF000; va += PAGE_SIZE) {
        if (run == 0) {
            if (va % alignment != 0) {
                continue;
            }
            start = va;
        }

        uint32_t* pt = vmm_get_pt(region, va);
        uint32_t pde = vmm_get_pde(region, va);
        int used = 0;

        if (pde & PAGE_PRESENT) {
            if (pt && (pt[pt_index(va)] & PAGE_PRESENT)) {
                used = 1;
            }
        }

        if (!used) {
            run++;
            if (run >= pages) {
                return start;
            }
        } else {
            run = 0;
            if (va % alignment != 0) {
                va = (va + alignment - 1) & ~(alignment - 1);
                va -= PAGE_SIZE;
            }
        }
    }
    return 0;
}

uintptr_t vmm_calloc(vmm_region_t* region, size_t pages, uint32_t flags) {
    uintptr_t va = vmm_alloc(region, pages, flags);
    if (va == (uintptr_t) -1) {
        return (uintptr_t) -1;
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t curr_va = va + i * PAGE_SIZE;
        flop_memset((void*) curr_va, 0, PAGE_SIZE);
    }
    return va;
}

uintptr_t vmm_alloc_aligned(vmm_region_t* region, size_t pages, size_t alignment, uint32_t flags) {
    if (alignment < PAGE_SIZE) {
        alignment = PAGE_SIZE;
    }

    uintptr_t va = vmm_find_free_range_aligned(region, pages, alignment);
    if (!va) {
        return 0;
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = (uintptr_t) pmm_alloc_page();
        if (!pa) {
            vmm_unmap_range(region, va, i);
            return 0;
        }
        vmm_map(region, va + i * PAGE_SIZE, pa, flags);
    }

    if (va + pages * PAGE_SIZE > region->next_free_va) {
        region->next_free_va = va + pages * PAGE_SIZE;
    }

    return va;
}

uintptr_t vmm_map_mmio(vmm_region_t* region, uintptr_t phys_addr, size_t size, uint32_t flags) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t aligned_phys = phys_addr & ~(PAGE_SIZE - 1);

    uintptr_t va = vmm_find_free_range(region, pages);
    if (!va) {
        return 0;
    }

    for (size_t i = 0; i < pages; i++) {
        if (vmm_map(region, va + i * PAGE_SIZE, aligned_phys + i * PAGE_SIZE, flags) < 0) {
            vmm_unmap_range(region, va, i);
            return 0;
        }
    }

    return va + (phys_addr & (PAGE_SIZE - 1));
}

int vmm_check_buffer(vmm_region_t* region, uintptr_t va, size_t size, bool write) {
    uintptr_t start_page = va & ~(PAGE_SIZE - 1);
    uintptr_t end_page = (va + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    size_t pages = (end_page - start_page) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t curr = start_page + i * PAGE_SIZE;
        uint32_t* pt = vmm_get_pt(region, curr);

        if (!pt) {
            return 0;
        }
        uint32_t entry = pt[pt_index(curr)];

        if (!(entry & PAGE_PRESENT)) {
            return 0;
        }
        if (!(entry & PAGE_USER)) {
            return 0;
        }
        if (write && !(entry & PAGE_RW)) {
            return 0;
        }
    }
    return 1;
}

int vmm_get_phys_list(vmm_region_t* region, uintptr_t va, size_t pages, uintptr_t* out_paddrs) {
    if (!out_paddrs) {
        return -1;
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(region, va + i * PAGE_SIZE);
        if (!pa) {
            return -1;
        }
        out_paddrs[i] = pa;
    }
    return 0;
}

int vmm_is_phys_contiguous(vmm_region_t* region, uintptr_t va, size_t pages) {
    uintptr_t prev_pa = vmm_resolve(region, va);
    if (!prev_pa) {
        return 0;
    }

    for (size_t i = 1; i < pages; i++) {
        uintptr_t curr_pa = vmm_resolve(region, va + i * PAGE_SIZE);
        if (!curr_pa) {
            return 0;
        }
        if (curr_pa != prev_pa + PAGE_SIZE) {
            return 0;
        }
        prev_pa = curr_pa;
    }
    return 1;
}

uintptr_t vmm_duplicate_page(vmm_region_t* region, uintptr_t va) {
    uintptr_t old_pa = vmm_resolve(region, va);
    if (!old_pa) {
        return 0;
    }

    uintptr_t new_pa = (uintptr_t) pmm_alloc_page();
    if (!new_pa) {
        return 0;
    }

    flop_memcpy((void*) new_pa, (void*) old_pa, PAGE_SIZE);

    uint32_t flags = 0;
    uint32_t* pt = vmm_get_pt(region, va);
    if (pt) {
        flags = pt[pt_index(va)] & 0xFFF;
    }

    vmm_map(region, va, new_pa, flags);
    return new_pa;
}

void vmm_flush_tlb(void) {
    asm volatile("mov %%cr3, %%eax\n"
                 "mov %%eax, %%cr3" ::
                     : "eax", "memory");
}

static bool _vmm_validator_dma(uintptr_t base, size_t size) {
    if ((base + size) > 0x01000000) {
        return false;
    }
    return true;
}

static bool _vmm_validator_kernel(uintptr_t base, size_t size) {
    if (base < (USER_SPACE_END + 1)) {
        return false;
    }
    return true;
}

void vmm_classes_init(vmm_region_t* region) {
    region->class_list = NULL;

    vmm_class_config_t k_conf = {.type = VM_CLASS_KERNEL,
                                 .name = "Kernel",
                                 .start = 0xC0000000,
                                 .end = 0xFFFFFFFF,
                                 .flags = PAGE_PRESENT | PAGE_RW,
                                 .align = PAGE_SIZE,
                                 .validator = _vmm_validator_kernel};
    vmm_class_register(region, &k_conf);

    vmm_class_config_t u_conf = {.type = VM_CLASS_USER,
                                 .name = "User",
                                 .start = USER_SPACE_START,
                                 .end = USER_SPACE_END,
                                 .flags = PAGE_PRESENT | PAGE_RW | PAGE_USER,
                                 .align = PAGE_SIZE,
                                 .validator = NULL};
    vmm_class_register(region, &u_conf);

    vmm_class_config_t dma_conf = {.type = VM_CLASS_DMA,
                                   .name = "DMA",
                                   .start = 0x1000,
                                   .end = 0x01000000,
                                   .flags = PAGE_PRESENT | PAGE_RW,
                                   .align = 0x10000,
                                   .validator = _vmm_validator_dma};
    vmm_class_register(region, &dma_conf);

    vmm_class_config_t mmio_conf = {.type = VM_CLASS_MMIO,
                                    .name = "MMIO",
                                    .start = 0xF0000000,
                                    .end = 0xF8000000,
                                    .flags = PAGE_PRESENT | PAGE_RW,
                                    .align = PAGE_SIZE,
                                    .validator = NULL};
    vmm_class_register(region, &mmio_conf);
}

int vmm_class_register(vmm_region_t* region, vmm_class_config_t* config) {
    if (!region || !config) {
        return -1;
    }

    vmm_alloc_class_t* new_class = (vmm_alloc_class_t*) kmalloc(sizeof(vmm_alloc_class_t));
    if (!new_class) {
        return -1;
    }

    flop_memcpy(&new_class->config, config, sizeof(vmm_class_config_t));
    new_class->current_ptr = config->start;

    new_class->next = region->class_list;
    region->class_list = new_class;

    return 0;
}

vmm_alloc_class_t* vmm_class_get(vmm_region_t* region, vm_class_type_t type) {
    vmm_alloc_class_t* iter = region->class_list;
    while (iter) {
        if (iter->config.type == type) {
            return iter;
        }
        iter = iter->next;
    }
    return NULL;
}

void vmm_class_destroy_all(vmm_region_t* region) {
    vmm_alloc_class_t* iter = region->class_list;
    while (iter) {
        vmm_alloc_class_t* next = iter->next;
        kfree(iter, sizeof(vmm_alloc_class_t));
        iter = next;
    }
    region->class_list = NULL;
}

uintptr_t vmm_class_alloc(vmm_region_t* region, vm_class_type_t type, size_t pages) {
    vmm_alloc_class_t* cls = vmm_class_get(region, type);
    if (!cls) {
        return 0;
    }

    size_t size = pages * PAGE_SIZE;
    uintptr_t ptr = cls->current_ptr;

    if (cls->config.align > PAGE_SIZE) {
        if ((ptr & (cls->config.align - 1)) != 0) {
            ptr = (ptr + cls->config.align - 1) & ~(cls->config.align - 1);
        }
    }

    uintptr_t start_of_search = ptr;
    bool wrapped = false;

    while (true) {
        if (ptr + size > cls->config.end) {
            if (wrapped) {
                return 0;
            }

            ptr = cls->config.start;
            if (cls->config.align > PAGE_SIZE) {
                ptr = (ptr + cls->config.align - 1) & ~(cls->config.align - 1);
            }
            wrapped = true;
            continue;
        }

        if (wrapped && ptr >= start_of_search) {
            return 0;
        }

        if (cls->config.validator && !cls->config.validator(ptr, size)) {
            ptr += PAGE_SIZE;
            continue;
        }

        bool is_free = true;
        for (size_t i = 0; i < pages; i++) {
            if (vmm_resolve(region, ptr + i * PAGE_SIZE) != 0) {
                is_free = false;
                break;
            }
        }

        if (is_free) {
            for (size_t i = 0; i < pages; i++) {
                uintptr_t pa = (uintptr_t) pmm_alloc_page();
                if (!pa) {
                    vmm_unmap_range(region, ptr, i);
                    return 0;
                }
                vmm_map(region, ptr + i * PAGE_SIZE, pa, cls->config.flags);
            }

            cls->current_ptr = ptr + size;
            return ptr;
        }

        ptr += PAGE_SIZE;
    }
}

uintptr_t vmm_alloc_kernel(vmm_region_t* region, size_t pages) {
    return vmm_class_alloc(region, VM_CLASS_KERNEL, pages);
}

uintptr_t vmm_alloc_user(vmm_region_t* region, size_t pages) {
    return vmm_class_alloc(region, VM_CLASS_USER, pages);
}

uintptr_t vmm_alloc_dma(vmm_region_t* region, size_t pages) {
    return vmm_class_alloc(region, VM_CLASS_DMA, pages);
}

uintptr_t vmm_alloc_mmio(vmm_region_t* region, size_t pages) {
    return vmm_class_alloc(region, VM_CLASS_MMIO, pages);
}
