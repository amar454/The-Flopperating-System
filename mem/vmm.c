#include <stdint.h>
#include "pmm.h"
#include "vmm.h"
#include "paging.h"
#include "utils.h"
#include "../cpu/cpu.h"
#include "../lib/logging.h"

extern uint32_t* pg_dir;
extern uint32_t* pg_tbls;
extern uint32_t* current_pg_dir;

static vmm_region_t* region_list = 0;
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
#define RECURSIVE_PT(pdi) ((uint32_t*) (RECURSIVE_ADDR + (pdi) *PAGE_SIZE))

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

void vmm_free(vmm_region_t* region, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(region, va + i * PAGE_SIZE);
        if (pa) {
            pmm_free_page((void*) pa);
        }
        vmm_unmap(region, va + i * PAGE_SIZE);
    }
}

int vmm_map(vmm_region_t* region, uintptr_t va, uintptr_t pa, uint32_t flags) {
    uint32_t pdi = pd_index(va);
    uint32_t pti = pt_index(va);

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

void region_insert(vmm_region_t* region) {
    region->next = region_list;
    region_list = region;
}

void region_remove(vmm_region_t* region) {
    vmm_region_t** iter = &region_list;
    while (*iter) {
        if (*iter == region) {
            *iter = region->next;
            break;
        } else {
            iter = &(*iter)->next;
        }
    }
}

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
    region->random_table = NULL;
    region->random_count = 0;
    region->random_capacity = 0;
    region->base_va = USER_SPACE_START;
    region->next_free_va = region->base_va;

    region_insert(region);

    if ((initial_pages > 0) && out_va) {
        uintptr_t va = vmm_alloc(region, initial_pages, flags);
        if (va == (uintptr_t) (-1)) {
            region_remove(region);
            kfree(region, sizeof(vmm_region_t));
            pmm_free_page((void*) dir_phys);
            return NULL;
        } else {
            *out_va = va;
        }
    }

    return region;
}

void vmm_region_destroy(vmm_region_t* region) {
    if (!region) {
        return;
    }
    region_remove(region);
    if (region->random_table) {
        kfree(region->random_table, region->random_capacity * sizeof(aslr_entry_t));
        region->random_table = NULL;
        region->random_count = 0;
        region->random_capacity = 0;
    }
    pmm_free_page((void*) region->pg_dir);
    kfree(region, sizeof(vmm_region_t));
}

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
    region_insert(&kernel_region);
    log("vmm: init - ok\n", GREEN);
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

vmm_region_t* vmm_copy_pagemap(vmm_region_t* src) {
    uint32_t* new_dir = vmm_new_copied_pgdir();
    if (!new_dir) {
        return 0;
    }
    vmm_region_t* dst = (vmm_region_t*) kmalloc(sizeof(vmm_region_t));
    if (!dst) {
        return 0;
    }
    dst->pg_dir = new_dir;
    dst->next = 0;
    dst->base_va = src->base_va;
    dst->next_free_va = src->next_free_va;

    for (int pdi = 0; pdi < 1024; pdi++) {
        if (!(src->pg_dir[pdi] & PAGE_PRESENT)) {
            continue;
        } else {
            uintptr_t pt_phys = (uintptr_t) pmm_alloc_page();
            if (!pt_phys) {
                vmm_region_destroy(dst);
                return 0;
            }
            uint32_t* src_pt = &pg_tbls[pdi * PAGE_ENTRIES];
            uint32_t* dst_pt = (uint32_t*) pt_phys;
            flop_memset(dst_pt, 0, PAGE_SIZE);

            for (int pti = 0; pti < PAGE_ENTRIES; pti++) {
                if (!(src_pt[pti] & PAGE_PRESENT)) {
                    continue;
                } else {
                    uintptr_t new_page = (uintptr_t) pmm_alloc_page();
                    if (!new_page) {
                        vmm_region_destroy(dst);
                        return 0;
                    } else {
                        flop_memcpy((void*) new_page, (void*) (src_pt[pti] & PAGE_MASK), PAGE_SIZE);
                        dst_pt[pti] = (new_page & PAGE_MASK) | (src_pt[pti] & ~PAGE_MASK);
                    }
                }
            }
            new_dir[pdi] = (pt_phys & PAGE_MASK) | (src->pg_dir[pdi] & ~PAGE_MASK);
        }
    }

    new_dir[RECURSIVE_PDE] = ((uintptr_t) new_dir & PAGE_MASK) | PAGE_PRESENT | PAGE_RW;
    region_insert(dst);
    return dst;
}

void vmm_nuke_pagemap(vmm_region_t* region) {
    for (int pdi = 0; pdi < 1024; pdi++) {
        if (!(region->pg_dir[pdi] & PAGE_PRESENT)) {
            continue;
        } else {
            uint32_t* pt = &pg_tbls[pdi * PAGE_ENTRIES];
            for (int pti = 0; pti < PAGE_ENTRIES; pti++) {
                if (pt[pti] & PAGE_PRESENT) {
                    pmm_free_page((void*) (pt[pti] & PAGE_MASK));
                }
            }
            uintptr_t pt_phys = region->pg_dir[pdi] & PAGE_MASK;
            pmm_free_page((void*) pt_phys);
        }
    }
    uintptr_t dir_phys = (uintptr_t) region->pg_dir;
    pmm_free_page((void*) dir_phys);
    region_remove(region);
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

int vmm_map_range(vmm_region_t* region, uintptr_t va, uintptr_t pa, size_t pages, uint32_t flags) {
    for (size_t i = 0; i < pages; i++) {
        if (vmm_map(region, va + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags) < 0) {
            return -1;
        }
    }
    return 0;
}

int vmm_unmap_range(vmm_region_t* region, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        if (vmm_unmap(region, va + i * PAGE_SIZE) < 0) {
            return -1;
        }
    }
    return 0;
}

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
        if (vmm_map(a, va_a + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags) < 0)
            return -1;
        if (vmm_map(b, va_b + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags) < 0)
            return -1;
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
    for (uintptr_t va = 0; va < KERNEL_VIRT_BASE; va += PAGE_SIZE)
        if (vmm_is_mapped(region, va))
            n++;
    return n;
}

static void vmm_aslr_init_region(vmm_region_t* region) {
    if (!region->random_table) {
        region->random_table = kmalloc(sizeof(aslr_entry_t) * 16);
        region->random_count = 0;
        region->random_capacity = 16;
    }
}

static void vmm_aslr_record(vmm_region_t* region, uintptr_t va, size_t pages, size_t align, uint32_t flags) {
    if (region->random_count >= region->random_capacity) {
        size_t new_cap = region->random_capacity * 2;
        region->random_table = krealloc(
            region->random_table, new_cap * sizeof(aslr_entry_t), region->random_capacity * sizeof(aslr_entry_t));
        region->random_capacity = new_cap;
    }

    aslr_entry_t* entry = &region->random_table[region->random_count++];
    entry->va = va;
    entry->pages = pages;
    entry->align = align;
    entry->flags = flags;
}

static uint32_t _vmm_aslr_rng_state = 0x12345678;

static uint32_t rand32(void) {
    _vmm_aslr_rng_state = _vmm_aslr_rng_state * 1664525 + 1013904223;
    return _vmm_aslr_rng_state;
}

static int aslr_table_grow(vmm_region_t* region, size_t want) {
    size_t newcap = region->random_capacity ? region->random_capacity * 2 : 8;
    while (newcap < want)
        newcap *= 2;

    aslr_entry_t* newtab = (aslr_entry_t*) kmalloc(newcap * sizeof(aslr_entry_t));
    if (!newtab)
        return -1;

    if (region->random_table) {
        flop_memcpy(newtab, region->random_table, region->random_count * sizeof(aslr_entry_t));
        kfree(region->random_table, region->random_capacity * sizeof(aslr_entry_t));
    }
    region->random_table = newtab;
    region->random_capacity = newcap;
    return 0;
}

uintptr_t vmm_aslr_alloc(vmm_region_t* region, size_t pages, size_t align, uint32_t flags) {
    if (!region || pages == 0)
        return 0;

    if (align == 0)
        align = 1;
    if ((align & (align - 1)) != 0) {
        size_t a = 1;
        while (a < align)
            a <<= 1;
        align = a;
    }
    size_t align_bytes = align * PAGE_SIZE;

    const int RANDOM_PROBES = 16;
    uintptr_t span = (USER_SPACE_END - USER_SPACE_START) & ~(PAGE_SIZE - 1);

    for (int probe = 0; probe < RANDOM_PROBES; ++probe) {
        uint32_t r = rand32();
        uintptr_t candidate_offset = ((uint64_t) r * (uint64_t) span) >> 32;
        uintptr_t candidate = (USER_SPACE_START + candidate_offset) & ~(align_bytes - 1);

        if (candidate < USER_SPACE_START)
            candidate = USER_SPACE_START;
        if (candidate + pages * PAGE_SIZE - 1 > USER_SPACE_END)
            continue;

        int ok = 1;
        for (size_t i = 0; i < pages; i++) {
            if (vmm_resolve(region, candidate + i * PAGE_SIZE)) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;

        if (region->random_count + 1 > region->random_capacity) {
            if (aslr_table_grow(region, region->random_count + 1) != 0)
                return 0;
        }
        aslr_entry_t* e = &region->random_table[region->random_count++];
        e->va = candidate;
        e->pages = pages;
        e->align = align;
        e->flags = flags;
        return candidate;
    }

    uintptr_t found = vmm_find_free_range(region, pages);
    if (!found)
        return 0;

    if (region->random_count + 1 > region->random_capacity) {
        if (aslr_table_grow(region, region->random_count + 1) != 0)
            return 0;
    }
    aslr_entry_t* e = &region->random_table[region->random_count++];
    e->va = found;
    e->pages = pages;
    e->align = align;
    e->flags = flags;
    return found;
}

void vmm_aslr_free(vmm_region_t* region, uintptr_t va) {
    if (!region)
        return;
    size_t idx = (size_t) -1;
    for (size_t i = 0; i < region->random_count; ++i) {
        if (region->random_table[i].va == va) {
            idx = i;
            break;
        }
    }
    if (idx == (size_t) -1)
        return;

    aslr_entry_t entry = region->random_table[idx];

    /* Just unmap the VA region (caller decides what to do with PA) */
    /* todo: free pages */
    vmm_unmap_range(region, entry.va, entry.pages);

    if (idx != region->random_count - 1) {
        region->random_table[idx] = region->random_table[region->random_count - 1];
    }
    region->random_count--;
}

int vmm_map_direct(vmm_region_t* region, uintptr_t phys, size_t pages, uint32_t flags) {
    for (size_t i = 0; i < pages; i++) {
        if (vmm_map(region, phys + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags) < 0)
            return -1;
    }
    return 0;
}

uintptr_t vmm_map_anonymous(vmm_region_t* region, size_t pages, uint32_t flags) {
    uintptr_t va = vmm_find_free_range(region, pages);
    if (!va)
        return 0;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = (uintptr_t) pmm_alloc_page();
        if (!pa) {
            for (size_t j = 0; j < i; j++)
                vmm_unmap(region, va + j * PAGE_SIZE);
            return 0;
        }
        if (vmm_map(region, va + i * PAGE_SIZE, pa, flags) < 0) {
            for (size_t j = 0; j <= i; j++)
                vmm_unmap(region, va + j * PAGE_SIZE);
            pmm_free_page((void*) pa);
            return 0;
        }
    }

    return va;
}

uintptr_t vmm_map_direct_aslr(vmm_region_t* region, uintptr_t phys, size_t pages, uint32_t flags, size_t align) {
    uintptr_t va = vmm_aslr_alloc(region, pages, align, flags);
    if (!va)
        return 0;

    for (size_t i = 0; i < pages; i++) {
        if (vmm_map(region, va + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags) < 0) {
            for (size_t j = 0; j < i; j++)
                vmm_unmap(region, va + j * PAGE_SIZE);
            vmm_aslr_free(region, va);
            return 0;
        }
    }

    return va;
}

uintptr_t vmm_map_anonymous_aslr(vmm_region_t* region, size_t pages, uint32_t flags, size_t align) {
    uintptr_t va = vmm_aslr_alloc(region, pages, align, flags);
    if (!va)
        return 0;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = (uintptr_t) pmm_alloc_page();
        if (!pa) {
            for (size_t j = 0; j < i; j++)
                vmm_unmap(region, va + j * PAGE_SIZE);
            vmm_aslr_free(region, va);
            return 0;
        }
        if (vmm_map(region, va + i * PAGE_SIZE, pa, flags) < 0) {
            for (size_t j = 0; j <= i; j++)
                vmm_unmap(region, va + j * PAGE_SIZE);
            pmm_free_page((void*) pa);
            vmm_aslr_free(region, va);
            return 0;
        }
    }

    return va;
}

int vmm_unmap_direct_aslr(vmm_region_t* region, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(region, va + i * PAGE_SIZE);
        if (!pa)
            continue;
        vmm_unmap(region, va + i * PAGE_SIZE);
    }
    vmm_aslr_free(region, va);
    return 0;
}

int vmm_unmap_anonymous_aslr(vmm_region_t* region, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(region, va + i * PAGE_SIZE);
        if (!pa)
            continue;
        vmm_unmap(region, va + i * PAGE_SIZE);
        pmm_free_page((void*) pa);
    }
    vmm_aslr_free(region, va);
    return 0;
}

static void shuffle_array(uintptr_t* array, size_t n) {
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rand32() % (i + 1);
        uintptr_t tmp = array[i];
        array[i] = array[j];
        array[j] = tmp;
    }
}

flop_randframe_region_t* rand_frames_create(vmm_region_t* region) {
    size_t pages = vmm_count_mapped(region);
    if (!pages)
        return NULL;

    vmm_region_t* table_region = vmm_region_create(0, PAGE_PRESENT | PAGE_RW | PAGE_USER, NULL);
    if (!table_region)
        return NULL;

    flop_randframe_region_t* rand_struct = (flop_randframe_region_t*) kmalloc(sizeof(flop_randframe_region_t));
    rand_struct->entries = (flop_rand_entry_t*) kmalloc(sizeof(flop_rand_entry_t) * pages);
    rand_struct->src_region = region;
    rand_struct->table_region = table_region;
    rand_struct->page_count = pages;

    uintptr_t* phys_pages = (uintptr_t*) kmalloc(sizeof(uintptr_t) * pages);
    size_t idx = 0;
    for (uintptr_t va = 0; va < 0xFFFFFFFF; va += PAGE_SIZE) {
        uintptr_t pa = vmm_resolve(region, va);
        if (!pa)
            continue;
        phys_pages[idx++] = pa;
        if (idx >= pages)
            break;
    }

    shuffle_array(phys_pages, pages);

    idx = 0;
    for (uintptr_t va = 0; va < 0xFFFFFFFF; va += PAGE_SIZE) {
        uintptr_t pa = vmm_resolve(region, va);
        if (!pa)
            continue;

        vmm_unmap(region, va);

        vmm_map(region, va, phys_pages[idx], PAGE_PRESENT | PAGE_RW);

        rand_struct->entries[idx].va = va;
        rand_struct->entries[idx].pa = phys_pages[idx];
        idx++;
    }

    kfree(phys_pages, sizeof(uintptr_t) * pages);
    return rand_struct;
}

void rand_frames_destroy(flop_randframe_region_t* rand_struct) {
    if (!rand_struct)
        return;

    for (size_t i = 0; i < rand_struct->page_count; i++) {
        vmm_unmap(rand_struct->src_region, rand_struct->entries[i].va);
        vmm_map(
            rand_struct->src_region, rand_struct->entries[i].va, rand_struct->entries[i].pa, PAGE_PRESENT | PAGE_RW);
    }

    vmm_region_destroy(rand_struct->table_region);
    kfree(rand_struct->entries, sizeof(flop_rand_entry_t) * rand_struct->page_count);
    kfree(rand_struct, sizeof(flop_randframe_region_t));
}

uintptr_t rand_frames_aslr_map_direct(
    flop_randframe_region_t* rand_struct, uintptr_t phys, size_t pages, uint32_t flags, size_t align) {
    uintptr_t va = vmm_aslr_alloc(rand_struct->src_region, pages, align, flags);
    if (!va)
        return 0;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = rand_struct->entries[i % rand_struct->page_count].pa;
        if (vmm_map(rand_struct->src_region, va + i * PAGE_SIZE, pa, flags) < 0) {
            for (size_t j = 0; j < i; j++)
                vmm_unmap(rand_struct->src_region, va + j * PAGE_SIZE);
            vmm_aslr_free(rand_struct->src_region, va);
            return 0;
        }
    }
    return va;
}

uintptr_t
rand_frames_aslr_map_anonymous(flop_randframe_region_t* rand_struct, size_t pages, uint32_t flags, size_t align) {
    uintptr_t va = vmm_aslr_alloc(rand_struct->src_region, pages, align, flags);
    if (!va)
        return 0;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = (uintptr_t) pmm_alloc_page();
        if (!pa) {
            for (size_t j = 0; j < i; j++)
                vmm_unmap(rand_struct->src_region, va + j * PAGE_SIZE);
            vmm_aslr_free(rand_struct->src_region, va);
            return 0;
        }
        pa = rand_struct->entries[i % rand_struct->page_count].pa;

        if (vmm_map(rand_struct->src_region, va + i * PAGE_SIZE, pa, flags) < 0) {
            for (size_t j = 0; j <= i; j++)
                vmm_unmap(rand_struct->src_region, va + j * PAGE_SIZE);
            pmm_free_page((void*) pa);
            vmm_aslr_free(rand_struct->src_region, va);
            return 0;
        }
    }
    return va;
}

int rand_frames_aslr_unmap_direct(flop_randframe_region_t* rand_struct, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(rand_struct->src_region, va + i * PAGE_SIZE);
        if (!pa)
            continue;
        vmm_unmap(rand_struct->src_region, va + i * PAGE_SIZE);
    }
    vmm_aslr_free(rand_struct->src_region, va);
    return 0;
}

int rand_frames_aslr_unmap_anonymous(flop_randframe_region_t* rand_struct, uintptr_t va, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(rand_struct->src_region, va + i * PAGE_SIZE);
        if (!pa)
            continue;
        vmm_unmap(rand_struct->src_region, va + i * PAGE_SIZE);
        pmm_free_page((void*) pa);
    }
    vmm_aslr_free(rand_struct->src_region, va);
    return 0;
}

int rand_frames_reshuffle(flop_randframe_region_t* rand_struct) {
    if (!rand_struct || !rand_struct->entries || !rand_struct->page_count)
        return -1;

    size_t pages = rand_struct->page_count;

    uintptr_t* phys_pages = (uintptr_t*) kmalloc(sizeof(uintptr_t) * pages);
    if (!phys_pages)
        return -1;

    for (size_t i = 0; i < pages; i++)
        phys_pages[i] = rand_struct->entries[i].pa;

    shuffle_array(phys_pages, pages);

    for (size_t i = 0; i < pages; i++) {
        uintptr_t va = rand_struct->entries[i].va;

        vmm_unmap(rand_struct->src_region, va);

        if (vmm_map(rand_struct->src_region, va, phys_pages[i], PAGE_PRESENT | PAGE_RW) < 0) {
            // rollback on failure
            for (size_t j = 0; j < i; j++) {
                vmm_unmap(rand_struct->src_region, rand_struct->entries[j].va);
                vmm_map(rand_struct->src_region,
                        rand_struct->entries[j].va,
                        rand_struct->entries[j].pa,
                        PAGE_PRESENT | PAGE_RW);
            }
            kfree(phys_pages, sizeof(uintptr_t) * pages);
            return -1;
        }

        rand_struct->entries[i].pa = phys_pages[i];
    }

    kfree(phys_pages, sizeof(uintptr_t) * pages);
    return 0;
}

int rand_frames_reshuffle_partial(flop_randframe_region_t* rand_struct, size_t start, size_t count) {
    if (!rand_struct || start >= rand_struct->page_count)
        return -1;
    if (start + count > rand_struct->page_count)
        count = rand_struct->page_count - start;

    uintptr_t* phys_pages = (uintptr_t*) kmalloc(sizeof(uintptr_t) * count);
    if (!phys_pages)
        return -1;

    for (size_t i = 0; i < count; i++)
        phys_pages[i] = rand_struct->entries[start + i].pa;

    shuffle_array(phys_pages, count);

    for (size_t i = 0; i < count; i++) {
        uintptr_t va = rand_struct->entries[start + i].va;
        vmm_unmap(rand_struct->src_region, va);

        if (vmm_map(rand_struct->src_region, va, phys_pages[i], PAGE_PRESENT | PAGE_RW) < 0) {
            for (size_t j = 0; j < i; j++) {
                vmm_unmap(rand_struct->src_region, rand_struct->entries[start + j].va);
                vmm_map(rand_struct->src_region,
                        rand_struct->entries[start + j].va,
                        rand_struct->entries[start + j].pa,
                        PAGE_PRESENT | PAGE_RW);
            }
            kfree(phys_pages, sizeof(uintptr_t) * count);
            return -1;
        }

        rand_struct->entries[start + i].pa = phys_pages[i];
    }

    kfree(phys_pages, sizeof(uintptr_t) * count);
    return 0;
}

flop_randframe_region_t* rand_frames_clone_exact(flop_randframe_region_t* src) {
    if (!src || !src->entries || !src->page_count)
        return NULL;

    vmm_region_t* table_region = vmm_region_create(0, PAGE_PRESENT | PAGE_RW | PAGE_USER, NULL);
    if (!table_region)
        return NULL;

    flop_randframe_region_t* clone = (flop_randframe_region_t*) kmalloc(sizeof(flop_randframe_region_t));
    if (!clone)
        return NULL;

    clone->entries = (flop_rand_entry_t*) kmalloc(sizeof(flop_rand_entry_t) * src->page_count);
    if (!clone->entries) {
        kfree(clone, sizeof(flop_randframe_region_t));
        return NULL;
    }

    clone->src_region = src->src_region;
    clone->table_region = table_region;
    clone->page_count = src->page_count;

    for (size_t i = 0; i < src->page_count; i++) {
        clone->entries[i].va = src->entries[i].va;
        clone->entries[i].pa = src->entries[i].pa;
    }

    return clone;
}

flop_randframe_region_t* rand_frames_clone_randomized(flop_randframe_region_t* src) {
    if (!src || !src->entries || !src->page_count)
        return NULL;

    vmm_region_t* table_region = vmm_region_create(0, PAGE_PRESENT | PAGE_RW | PAGE_USER, NULL);
    if (!table_region)
        return NULL;

    flop_randframe_region_t* clone = (flop_randframe_region_t*) kmalloc(sizeof(flop_randframe_region_t));
    if (!clone)
        return NULL;

    clone->entries = (flop_rand_entry_t*) kmalloc(sizeof(flop_rand_entry_t) * src->page_count);
    if (!clone->entries) {
        kfree(clone, sizeof(flop_randframe_region_t));
        return NULL;
    }
    clone->src_region = src->src_region;
    clone->table_region = table_region;
    clone->page_count = src->page_count;
    for (size_t i = 0; i < src->page_count; i++)
        clone->entries[i].va = src->entries[i].va;
    uintptr_t* phys_pages = (uintptr_t*) kmalloc(sizeof(uintptr_t) * src->page_count);
    if (!phys_pages) {
        kfree(clone->entries, sizeof(flop_rand_entry_t) * src->page_count);
        kfree(clone, sizeof(flop_randframe_region_t));
        return NULL;
    }

    for (size_t i = 0; i < src->page_count; i++)
        phys_pages[i] = src->entries[i].pa;

    shuffle_array(phys_pages, src->page_count);

    for (size_t i = 0; i < src->page_count; i++) {
        clone->entries[i].pa = phys_pages[i];
        vmm_unmap(clone->src_region, clone->entries[i].va);
        vmm_map(clone->src_region, clone->entries[i].va, phys_pages[i], PAGE_PRESENT | PAGE_RW);
    }

    kfree(phys_pages, sizeof(uintptr_t) * src->page_count);
    return clone;
}

uintptr_t rand_frames_clone_aslr_map_direct(
    flop_randframe_region_t* clone, uintptr_t phys, size_t pages, uint32_t flags, size_t align) {
    if (!clone || !clone->entries)
        return 0;

    uintptr_t va = vmm_aslr_alloc(clone->src_region, pages, align, flags);
    if (!va)
        return 0;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = clone->entries[i % clone->page_count].pa;
        if (vmm_map(clone->src_region, va + i * PAGE_SIZE, pa, flags) < 0) {
            for (size_t j = 0; j < i; j++) {
                vmm_unmap(clone->src_region, va + j * PAGE_SIZE);
            }
            vmm_aslr_free(clone->src_region, va);
            return 0;
        }
    }
    return va;
}

uintptr_t
rand_frames_clone_aslr_map_anonymous(flop_randframe_region_t* clone, size_t pages, uint32_t flags, size_t align) {
    if (!clone || !clone->entries)
        return 0;

    uintptr_t va = vmm_aslr_alloc(clone->src_region, pages, align, flags);
    if (!va)
        return 0;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = (uintptr_t) pmm_alloc_page();
        if (!pa) {
            for (size_t j = 0; j < i; j++)
                vmm_unmap(clone->src_region, va + j * PAGE_SIZE);
            vmm_aslr_free(clone->src_region, va);
            return 0;
        }

        // replace with a randomized PA from the clone table
        pa = clone->entries[i % clone->page_count].pa;

        if (vmm_map(clone->src_region, va + i * PAGE_SIZE, pa, flags) < 0) {
            for (size_t j = 0; j <= i; j++)
                vmm_unmap(clone->src_region, va + j * PAGE_SIZE);
            pmm_free_page((void*) pa);
            vmm_aslr_free(clone->src_region, va);
            return 0;
        }
    }
    return va;
}

int rand_frames_clone_aslr_unmap_direct(flop_randframe_region_t* clone, uintptr_t va, size_t pages) {
    if (!clone)
        return -1;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(clone->src_region, va + i * PAGE_SIZE);
        if (!pa)
            continue;
        vmm_unmap(clone->src_region, va + i * PAGE_SIZE);
    }
    vmm_aslr_free(clone->src_region, va);
    return 0;
}

int rand_frames_clone_aslr_unmap_anonymous(flop_randframe_region_t* clone, uintptr_t va, size_t pages) {
    if (!clone)
        return -1;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t pa = vmm_resolve(clone->src_region, va + i * PAGE_SIZE);
        if (!pa)
            continue;
        vmm_unmap(clone->src_region, va + i * PAGE_SIZE);
        pmm_free_page((void*) pa);
    }
    vmm_aslr_free(clone->src_region, va);
    return 0;
}
