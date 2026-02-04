#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "../multiboot/multiboot.h"
#include "paging.h"
#include "../task/sync/spinlock.h"
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define MAX_ORDER 10
#define PMM_HAS_MMAP(mb) ((mb) && ((mb)->flags & MULTIBOOT_INFO_MEM_MAP))
#define PMM_HAS_MODS(mb) ((mb) && ((mb)->flags & MULTIBOOT_INFO_MODS))
#define PMM_MMAP_BEGIN(mb) ((uint8_t*) (uintptr_t) (mb)->mmap_addr)
#define PMM_MMAP_END(mb) (PMM_MMAP_BEGIN(mb) + (mb)->mmap_length)
#define PMM_MMAP_ENTRY(ptr) ((multiboot_memory_map_t*) (void*) (ptr))
#define PMM_MMAP_ENTRY_VALID(entry) ((entry) && (entry)->size != 0)
#define PMM_MMAP_NEXT(entry) ((uint8_t*) (entry) + (entry)->size + sizeof((entry)->size))
#define PMM_REGION_USABLE(entry) ((entry)->type == MULTIBOOT_MEMORY_AVAILABLE && (entry)->addr >= 0x100000ULL)
#define PMM_ALIGN(x) (align_up((x), PAGE_SIZE))
#define PMM_REGION_START(entry) (PMM_ALIGN((uintptr_t) (entry)->addr))
#define PMM_REGION_END(entry) (((uintptr_t) (entry)->addr + (uintptr_t) (entry)->len) & ~(PAGE_SIZE - 1))
#define PAGE_SIZE 4096
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

struct page {
    uintptr_t address;
    uint32_t order;
    int is_free;
    struct page* next;
};

struct buddy_allocator {
    struct page* free_list[MAX_ORDER + 1];
    struct page* page_info;
    uint32_t total_pages;
    uintptr_t memory_start;
    uintptr_t memory_end;
    uint32_t memory_base;
    spinlock_t lock;
};

extern uint32_t* pg_dir;
extern uint32_t* pg_tbls;
extern struct buddy_allocator buddy;

void pmm_init(multiboot_info_t* mb_info);
void* pmm_alloc_pages(uint32_t order, uint32_t count);
void* pmm_alloc_page(void);
void pmm_free_pages(void* addr, uint32_t order, uint32_t count);
void pmm_free_page(void* addr);
uint32_t pmm_get_memory_size();
uint32_t pmm_get_page_count();
struct page* phys_to_page_index(uintptr_t addr);
uint32_t page_index(uintptr_t addr);
void pmm_copy_page(void* dst, void* src);
int pmm_is_valid_addr(uintptr_t addr);
uintptr_t pmm_get_buddy_address(uintptr_t addr, uint32_t order);
bool pmm_is_primary_buddy(uintptr_t addr, uint32_t order);
size_t pmm_get_block_size(uint32_t order);
bool pmm_is_page_free(uintptr_t addr);
uint32_t pmm_get_page_order(uintptr_t addr);
uint32_t pmm_count_free_of_order(uint32_t order);
uintptr_t pmm_align_to_order(uintptr_t addr, uint32_t order);
bool pmm_check_alignment(uintptr_t addr, uint32_t order);
#endif
