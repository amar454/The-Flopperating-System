#include "pmm.h"
#include "../drivers/vga/vgahandler.h"
#include "../drivers/time/floptime.h"
#include "../multiboot/multiboot.h"
#include "../lib/logging.h"
#include "utils.h"
#include "paging.h"
#include "../apps/echo.h"
#include "pmm.h"
#include "alloc.h"
#include "early.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static struct early_info early;

#define EARLY_BIT_SET(i) (early.bitmap[(i) >> 3] |= (1u << ((i) & 7)))
#define EARLY_BIT_CLEAR(i) (early.bitmap[(i) >> 3] &= ~(1u << ((i) & 7)))
#define EARLY_BIT_TEST(i) (early.bitmap[(i) >> 3] & (1u << ((i) & 7)))

#define PAGE_SIZE 4096
#define EARLY_PAGES_TOTAL 5

static void* early_reserved[EARLY_PAGES_TOTAL];
static uint32_t early_reserved_count = 0;
static multiboot_info_t* early_mb_info = NULL;

static inline uintptr_t align_up(uintptr_t x, uintptr_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

void early_bootstrap(multiboot_info_t* mb) {
    early_mb_info = mb;
}

void* early_reserve_page(void) {
    if (!early_mb_info || early_reserved_count >= EARLY_PAGES_TOTAL) {
        return NULL;
    }

    uint8_t* ptr = (uint8_t*) (uintptr_t) early_mb_info->mmap_addr;
    uint8_t* end = ptr + early_mb_info->mmap_length;

    while (ptr < end) {
        multiboot_memory_map_t* mm = (multiboot_memory_map_t*) ptr;

        if (mm->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uintptr_t region_start = align_up((uintptr_t) mm->addr, PAGE_SIZE);
            uintptr_t region_end = (uintptr_t) mm->addr + (uintptr_t) mm->len;

            for (uintptr_t p = region_start; p + PAGE_SIZE <= region_end; p += PAGE_SIZE) {
                bool already_taken = false;

                for (uint32_t i = 0; i < early_reserved_count; i++) {
                    if ((uintptr_t) early_reserved[i] == p) {
                        already_taken = true;
                        break;
                    }
                }

                if (already_taken) {
                    continue;
                }

                void* page = (void*) p;
                early_reserved[early_reserved_count++] = page;

                uint8_t* b = (uint8_t*) page;
                for (uint32_t i = 0; i < PAGE_SIZE; i++) {
                    b[i] = 0;
                }

                return page;
            }
        }

        ptr += mm->size + sizeof(mm->size);
    }

    return NULL;
}

void early_release_page(void* p) {
    if (!p) {
        return;
    }

    for (uint32_t i = 0; i < early_reserved_count; i++) {
        if (early_reserved[i] == p) {
            early_reserved[i] = early_reserved[early_reserved_count - 1];
            early_reserved_count--;
            return;
        }
    }
}

void early_allocator_init(void) {
    if (early.initialized) {
        return;
    }

    void* meta_page = early_reserve_page();
    void* first_pool = NULL;

    for (int i = 0; i < EARLY_POOL_PAGES; i++) {
        void* p = early_reserve_page();
        if (i == 0) {
            first_pool = p;
        }
    }

    early.pool_base = (uint8_t*) first_pool;
    early.blocks_total = (EARLY_POOL_PAGES * PAGE_SIZE) / EARLY_BLOCK_SIZE;
    early.blocks_free = early.blocks_total;

    for (uint32_t i = 0; i < sizeof(early.bitmap); i++) {
        early.bitmap[i] = 0;
    }

    early.initialized = true;
}

void* early_alloc(size_t size) {
    if (!early.initialized) {
        return NULL;
    }

    if (size == 0 || size > EARLY_BLOCK_SIZE) {
        return NULL;
    }

    for (uint32_t i = 0; i < early.blocks_total; i++) {
        if (!EARLY_BIT_TEST(i)) {
            EARLY_BIT_SET(i);
            early.blocks_free--;
            return early.pool_base + (i * EARLY_BLOCK_SIZE);
        }
    }

    return NULL;
}

void early_free(void* ptr) {
    if (!early.initialized || !ptr) {
        return;
    }

    uintptr_t off = (uintptr_t) ptr - (uintptr_t) early.pool_base;

    if (off % EARLY_BLOCK_SIZE != 0) {
        return;
    }

    uint32_t idx = off / EARLY_BLOCK_SIZE;

    if (idx >= early.blocks_total) {
        return;
    }

    if (EARLY_BIT_TEST(idx)) {
        EARLY_BIT_CLEAR(idx);
        early.blocks_free++;
    }
}

void early_allocator_destroy(void) {
    // cannot destroy uninitialized allocator
    if (!early.initialized) {
        return;
    }

    // set all bits to zero
    for (uint32_t i = 0; i < sizeof(early.bitmap); i++) {
        early.bitmap[i] = 0;
    }

    uint8_t* p = early.pool_base;

    // release all pages
    for (uint32_t pg = 0; pg < EARLY_POOL_PAGES; pg++) {
        for (uint32_t i = 0; i < PAGE_SIZE; i++) {
            p[i] = 0;
        }
        early_release_page(p);
        p += PAGE_SIZE;
    }

    early.pool_base = NULL;
    early.blocks_total = 0;
    early.blocks_free = 0;
    early.initialized = false;
}
