/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
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
    // if we dont have memory map info or no more pages available
    // fail
    if (!early_mb_info || early_reserved_count >= EARLY_PAGES_TOTAL) {
        return NULL;
    }

    // iterate through mmap
    uint8_t* ptr = (uint8_t*) (uintptr_t) early_mb_info->mmap_addr;
    uint8_t* end = ptr + early_mb_info->mmap_length;

    while (ptr < end) {
        multiboot_memory_map_t* mm = (multiboot_memory_map_t*) ptr;

        // only allocate from available memory
        if (mm->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uintptr_t region_start = align_up((uintptr_t) mm->addr, PAGE_SIZE);
            uintptr_t region_end = (uintptr_t) mm->addr + (uintptr_t) mm->len;

            // walk through region
            for (uintptr_t p = region_start; p + PAGE_SIZE <= region_end; p += PAGE_SIZE) {
                bool already_taken = false;

                // make sure we dont return the same page twice
                for (uint32_t i = 0; i < early_reserved_count; i++) {
                    if ((uintptr_t) early_reserved[i] == p) {
                        already_taken = true;
                        break;
                    }
                }

                if (already_taken) {
                    continue;
                }

                // reserve page
                void* page = (void*) p;
                early_reserved[early_reserved_count++] = page;

                // zero page
                uint8_t* b = (uint8_t*) page;
                for (uint32_t i = 0; i < PAGE_SIZE; i++) {
                    b[i] = 0;
                }

                return page;
            }
        }

        // move to next entry
        ptr += mm->size + sizeof(mm->size);
    }

    // no page found
    return NULL;
}

void early_release_page(void* p) {
    if (!p) {
        return;
    }

    // find reserved entry
    // and remove it from the list
    for (uint32_t i = 0; i < early_reserved_count; i++) {
        if (early_reserved[i] == p) {
            // swap last entry down
            early_reserved[i] = early_reserved[early_reserved_count - 1];
            early_reserved_count--;
            return;
        }
    }
}

void early_allocator_init(void) {
    // avoid double initialization
    if (early.initialized) {
        return;
    }

    // reserve page for metadata
    void* meta_page = early_reserve_page();
    void* first_pool = NULL;

    // reserve contiguous pool pages
    for (int i = 0; i < EARLY_POOL_PAGES; i++) {
        void* p = early_reserve_page();
        if (i == 0) {
            first_pool = p;
        }
    }

    // init allocator state
    early.pool_base = (uint8_t*) first_pool;

    early_head = (early_chunk_t*) early.pool_base;
    early_head->size = (EARLY_POOL_PAGES * PAGE_SIZE) - CHUNK_HEADER_SIZE;
    early_head->free = true;
    early_head->next = NULL;
    early_head->prev = NULL;

    early.initialized = true;
}

void* early_alloc(size_t size) {
    if (!early.initialized || size == 0) {
        return NULL;
    }

    early_chunk_t* cur = early_head;

    while (cur) {
        if (cur->free && cur->size >= size) {
            size_t remaining = cur->size - size;

            // split if enough room remains for another chunk
            if (remaining > CHUNK_HEADER_SIZE + 8) {
                early_chunk_t* new_chunk = (early_chunk_t*) ((uint8_t*) cur + CHUNK_HEADER_SIZE + size);

                new_chunk->size = remaining - CHUNK_HEADER_SIZE;
                new_chunk->free = true;
                new_chunk->next = cur->next;
                new_chunk->prev = cur;

                if (cur->next) {
                    cur->next->prev = new_chunk;
                }

                cur->next = new_chunk;
                cur->size = size;
            }

            cur->free = false;
            return (uint8_t*) cur + CHUNK_HEADER_SIZE;
        }

        cur = cur->next;
    }

    return NULL; // out of memory
}

static void early_coalesce(early_chunk_t* chunk) {
    // merge forward
    if (chunk->next && chunk->next->free) {
        chunk->size += CHUNK_HEADER_SIZE + chunk->next->size;
        chunk->next = chunk->next->next;
        if (chunk->next) {
            chunk->next->prev = chunk;
        }
    }

    // merge backward
    if (chunk->prev && chunk->prev->free) {
        chunk->prev->size += CHUNK_HEADER_SIZE + chunk->size;
        chunk->prev->next = chunk->next;
        if (chunk->next) {
            chunk->next->prev = chunk->prev;
        }
    }
}

void early_free(void* ptr) {
    if (!early.initialized || !ptr) {
        return;
    }

    early_chunk_t* chunk = (early_chunk_t*) ((uint8_t*) ptr - CHUNK_HEADER_SIZE);

    chunk->free = true;
    early_coalesce(chunk);
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
