#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include "../task/sync/spinlock.h"
#define PAGE_SIZE 4096
#define RECURSIVE_PDE 1023

#define RECURSIVE_ADDR 0xFFC00000
#define RECURSIVE_PT(pdi) ((uint32_t*) (RECURSIVE_ADDR + (pdi) * PAGE_SIZE))

extern uint32_t* pg_dir;
extern uint32_t* pg_tbls;
extern uint32_t* current_pg_dir;

typedef enum {
    VM_CLASS_KERNEL,
    VM_CLASS_USER,
    VM_CLASS_MMIO,
    VM_CLASS_DMA,
    VM_CLASS_STACK,
    VM_CLASS_CUSTOM
} vm_class_type_t;

typedef struct vmm_class_config {
    vm_class_type_t type;
    const char* name;
    uintptr_t start;
    uintptr_t end;
    uint32_t flags;
    size_t align;
    bool (*validator)(uintptr_t base, size_t size);
} vmm_class_config_t;

typedef struct vmm_alloc_class {
    vmm_class_config_t config;
    uintptr_t current_ptr;
    spinlock_t lock;
    struct vmm_alloc_class* next;
} vmm_alloc_class_t;

typedef struct vmm_region {
    uint32_t* pg_dir;
    struct vmm_region* next;
    uintptr_t base_va;
    uintptr_t next_free_va;
    struct vmm_alloc_class* class_list;
} vmm_region_t;

void vmm_classes_init(vmm_region_t* region);
int vmm_class_register(vmm_region_t* region, vmm_class_config_t* config);
uintptr_t vmm_class_alloc(vmm_region_t* region, vm_class_type_t type, size_t pages);
vmm_alloc_class_t* vmm_class_get(vmm_region_t* region, vm_class_type_t type);
void vmm_class_destroy_all(vmm_region_t* region);
uintptr_t vmm_alloc_kernel(vmm_region_t* region, size_t pages);
uintptr_t vmm_alloc_user(vmm_region_t* region, size_t pages);
uintptr_t vmm_alloc_dma(vmm_region_t* region, size_t pages);
uintptr_t vmm_alloc_mmio(vmm_region_t* region, size_t pages);
int vmm_alloc_pde(uint32_t* dir, uint32_t pde_idx, uint32_t flags);
#define USER_SPACE_START 0x00100000U
#define USER_SPACE_END 0xBFFFFFFFU
void vmm_region_insert(vmm_region_t* region);
void vmm_region_remove(vmm_region_t* region);
uintptr_t vmm_resolve(vmm_region_t* region, uintptr_t va);
int vmm_map(vmm_region_t* region, uintptr_t va, uintptr_t pa, uint32_t flags);
int vmm_unmap(vmm_region_t* region, uintptr_t va);
int vmm_map_range(vmm_region_t* region, uintptr_t va, uintptr_t pa, size_t pages, uint32_t flags);
int vmm_unmap_range(vmm_region_t* region, uintptr_t va, size_t pages);
uintptr_t vmm_find_free_range(vmm_region_t* region, size_t pages);
int vmm_protect(vmm_region_t* region, uintptr_t va, uint32_t flags);
vmm_region_t* vmm_region_create(size_t initial_pages, uint32_t flags, uintptr_t* out_va);
void vmm_region_destroy(vmm_region_t* region);
void vmm_switch(vmm_region_t* region);
uintptr_t vmm_alloc(vmm_region_t* region, size_t pages, uint32_t flags);
void vmm_free(vmm_region_t* region, uintptr_t va, size_t pages);
void vmm_init();
int vmm_protect_range(vmm_region_t* region, uintptr_t va, size_t pages, uint32_t flags);
uintptr_t vmm_alloc_stack(vmm_region_t* region, size_t pages, uint32_t flags);
int vmm_map_scatter(vmm_region_t* region, uintptr_t va, uintptr_t* phys_pages, size_t pages, uint32_t flags);
int vmm_is_range_mapped(vmm_region_t* region, uintptr_t va, size_t pages);
uint32_t vmm_get_flags(vmm_region_t* region, uintptr_t va);
void vmm_dump_map(vmm_region_t* region);
vmm_region_t* vmm_copy_pagemap(vmm_region_t* src);
vmm_region_t* vmm_get_current();
uintptr_t vmm_calloc(vmm_region_t* region, size_t pages, uint32_t flags);
uintptr_t vmm_alloc_aligned(vmm_region_t* region, size_t pages, size_t alignment, uint32_t flags);
uintptr_t vmm_map_mmio(vmm_region_t* region, uintptr_t phys_addr, size_t size, uint32_t flags);
int vmm_check_buffer(vmm_region_t* region, uintptr_t va, size_t size, bool write);
int vmm_get_phys_list(vmm_region_t* region, uintptr_t va, size_t pages, uintptr_t* out_paddrs);
int vmm_is_phys_contiguous(vmm_region_t* region, uintptr_t va, size_t pages);
uintptr_t vmm_duplicate_page(vmm_region_t* region, uintptr_t va);
void vmm_flush_tlb(void);
void vmm_free_physical_frames(uint32_t* pt);
void vmm_iterate_through_page_tables(vmm_region_t* region);
void vmm_nuke_pagemap(vmm_region_t* region);
int vmm_copy_frames(uint32_t* src_pt, uint32_t* dst_pt);
int vmm_iterate_and_copy_page_tables(vmm_region_t* src, vmm_region_t* dst);
vmm_region_t* vmm_copy_pagemap(vmm_region_t* src);
int vmm_is_mapped(vmm_region_t* region, uintptr_t va);
int vmm_is_user_mapped(vmm_region_t* region, uintptr_t va);
int vmm_is_kernel_mapped(vmm_region_t* region, uintptr_t va);

#endif // VMM_H
