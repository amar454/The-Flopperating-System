/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later veregion_startion.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

*/
#include "pmm.h"
#include "vmm.h"
#include "alloc.h"
#include "utils.h"
#include "../lib/logging.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static spinlock_t heap_lock;
static box_t* boxes = NULL;
static int heap_initialized = 0;

static inline uint32_t box_hash_resolve_index(uint32_t id) {
    return (id * 2654435761u) & BOX_HASH_MASK;
}

static void box_hash_insert(uint32_t id, box_t* box) {
    uint32_t idx = box_hash_resolve_index(id);

    for (;;) {
        if (!box_hash[idx].value) {
            box_hash[idx].key = id;
            box_hash[idx].value = box;
            return;
        }
        idx = (idx + 1) & BOX_HASH_MASK;
    }
}

static void box_hash_remove(uint32_t id) {
    uint32_t idx = box_hash_resolve_index(id);

    for (;;) {
        if (!box_hash[idx].value) {
            return;
        }
        if (box_hash[idx].key == id) {
            box_hash[idx].value = NULL;
            return;
        }
        idx = (idx + 1) & BOX_HASH_MASK;
    }
}

static void box_init(box_t* box, void* page) {
    uintptr_t base = (uintptr_t) page;

    spinlock_init(&box->lock);

    box->page = page;
    box->total_blocks = BLOCKS_PER_BOX;
    box->map = (uint8_t*) (base + sizeof(box_t));
    box->data_pointer = (void*) (base + sizeof(box_t) + ((BLOCKS_PER_BOX + 7) / 8));

    flop_memset(box->map, 0, (BLOCKS_PER_BOX + 7) / 8);
}

static void box_hash_insert(uint32_t id, box_t* box);

static void heap_box_register(box_t* box) {
    spinlock(&heap_lock);

    box->id = next_box_id++;
    box->next = boxes;
    boxes = box;

    box_hash_insert(box->id, box);

    spinlock_unlock(&heap_lock, true);
}

static box_t* heap_create_box(void) {
    void* page = pmm_alloc_page();
    if (!page) {
        return NULL;
    }

    box_t* box = (box_t*) page;
    box_init(box, page);
    heap_box_register(box);
    return box;
}

static int heap_map_find_free(uint8_t* map, int total, int needed) {
    int run = 0;
    int start = -1;

    for (int i = 0; i < total; i++) {
        int byte = i / 8;
        int bit = i % 8;

        if (!(map[byte] & (1 << bit))) {
            if (run == 0) {
                start = i;
            }
            if (++run >= needed) {
                return start;
            }
        } else {
            run = 0;
            start = -1;
        }
    }
    return -1;
}

static void heap_map_set(uint8_t* map, int start, int count, bool used) {
    for (int i = start; i < start + count; i++) {
        int byte = i / 8;
        int bit = i % 8;

        if (used) {
            map[byte] |= (1 << bit);
        } else {
            map[byte] &= ~(1 << bit);
        }
    }
}

static void* heap_box_alloc(box_t* box, size_t size) {
    spinlock(&box->lock);

    size_t total = size + OBJECT_ALIGN;
    int needed = (total + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int start = heap_map_find_free(box->map, box->total_blocks, needed);

    if (start < 0) {
        spinlock_unlock(&box->lock, true);
        return NULL;
    }

    heap_map_set(box->map, start, needed, true);

    uintptr_t mem = (uintptr_t) box->data_pointer + start * BLOCK_SIZE;

    object_t* obj = (object_t*) mem;
    obj->box = box;
    obj->size = size;

    spinlock_unlock(&box->lock, true);
    return (void*) (mem + OBJECT_ALIGN);
}

static bool heap_box_is_empty(box_t* box) {
    int bytes = (box->total_blocks + 7) / 8;
    for (int i = 0; i < bytes; i++) {
        if (box->map[i] != 0) {
            return false;
        }
    }
    return true;
}

static void box_hash_remove(uint32_t id);

static void heap_box_free(box_t* box) {
    if (!box) {
        return;
    }

    spinlock(&box->lock);

    if (!heap_box_is_empty(box)) {
        spinlock_unlock(&box->lock, true);
        return;
    }

    spinlock_unlock(&box->lock, true);

    spinlock(&heap_lock);

    box_t** it = &boxes;
    while (*it) {
        if (*it == box) {
            *it = box->next;
            break;
        }
        it = &(*it)->next;
    }

    box_hash_remove(box->id);

    spinlock_unlock(&heap_lock, true);

    pmm_free_page(box->page);
}

static int heap_fetch_block_index(box_t* box, void* mem) {
    uintptr_t base = (uintptr_t) box->data_pointer;
    uintptr_t p = (uintptr_t) mem;

    if (p < base) {
        return -1;
    }

    uintptr_t diff = p - base;

    if (diff % BLOCK_SIZE) {
        return -1;
    }

    int idx = diff / BLOCK_SIZE;
    return (idx >= 0 && idx < box->total_blocks) ? idx : -1;
}

void heap_init(void) {
    if (heap_initialized) {
        return;
    }

    spinlock_init(&heap_lock);
    boxes = NULL;

    if (!heap_create_box()) {
        return;
    }

    heap_initialized = 1;
    log("heap: init - ok\n", GREEN);
}

static void* heap_box_iterate(size_t size) {
    spinlock(&heap_lock);
    for (box_t* b = boxes; b; b = b->next) {
        void* r = heap_box_alloc(b, size);
        if (r) {
            spinlock_unlock(&heap_lock, true);
            return r;
        }
    }
    spinlock_unlock(&heap_lock, true);
    return NULL;
}

static box_t* box_hash_lookup(uint32_t id) {
    uint32_t idx = box_hash_resolve_index(id);

    for (;;) {
        if (!box_hash[idx].value) {
            return NULL;
        }
        if (box_hash[idx].key == id) {
            return box_hash[idx].value;
        }
        idx = (idx + 1) & BOX_HASH_MASK;
    }
}

void* kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    if (size > PAGE_SIZE) {
        size_t pages = (size + OBJECT_ALIGN + PAGE_SIZE - 1) / PAGE_SIZE;
        void* mem = pmm_alloc_pages(0, pages);
        if (!mem) {
            return NULL;
        }

        object_t* obj = (object_t*) mem;
        obj->box = NULL;
        obj->size = size;
        return (void*) ((uintptr_t) mem + OBJECT_ALIGN);
    }

    if (!heap_initialized) {
        heap_init();
    }

    void* r = heap_box_iterate(size);
    if (r) {
        return r;
    }

    box_t* b = heap_create_box();
    return b ? heap_box_alloc(b, size) : NULL;
}

void kfree(void* ptr, size_t size) {
    if (!ptr) {
        return;
    }

    object_t* obj = (object_t*) ((uintptr_t) ptr - OBJECT_ALIGN);

    if (!obj->box) {
        size_t pages = (obj->size + OBJECT_ALIGN + PAGE_SIZE - 1) / PAGE_SIZE;
        pmm_free_pages((void*) obj, 0, pages);
        return;
    }

    box_t* b = obj->box;
    int needed = (obj->size + OBJECT_ALIGN + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int idx = heap_fetch_block_index(b, obj);

    if (idx >= 0) {
        heap_map_set(b->map, idx, needed, false);
        heap_box_free(b);
    }
}

void* kcalloc(size_t n, size_t s) {
    size_t t = n * s;
    void* p = kmalloc(t);
    if (p) {
        flop_memset(p, 0, t);
    }
    return p;
}

void* krealloc(void* ptr, size_t new_size, size_t old_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }

    if (new_size == 0) {
        kfree(ptr, old_size);
        return NULL;
    }

    object_t* old = (object_t*) ((uintptr_t) ptr - OBJECT_ALIGN);
    void* n = kmalloc(new_size);
    if (!n) {
        return NULL;
    }

    size_t copy = old->size < new_size ? old->size : new_size;
    flop_memcpy(n, ptr, copy);
    kfree(ptr, old_size);
    return n;
}

void* kmalloc_guarded(size_t size) {
    if (size == 0) {
        return NULL;
    }

    size_t data_pages = (size + sizeof(guarded_object_t) + PAGE_SIZE - 1) / PAGE_SIZE;

    size_t total_pages = data_pages + 1;
    void* base = pmm_alloc_pages(0, total_pages);
    if (!base) {
        return NULL;
    }

    uintptr_t guard_va = (uintptr_t) base + data_pages * PAGE_SIZE;
    vmm_unmap(vmm_get_current(), guard_va);

    guarded_object_t* obj = (guarded_object_t*) base;
    obj->size = size;
    obj->pages = total_pages;

    return (void*) ((uintptr_t) base + sizeof(guarded_object_t));
}

void kfree_guarded(void* ptr) {
    if (!ptr) {
        return;
    }

    guarded_object_t* obj = (guarded_object_t*) ((uintptr_t) ptr - sizeof(guarded_object_t));

    pmm_free_pages((void*) obj, 0, obj->pages);
}

int kmalloc_memtest(void) {
    void* a = kmalloc(64);
    if (!a) {
        return -1;
    }
    kfree(a, 64);

    void* b = kmalloc(64);
    if (!b) {
        return -1;
    }
    kfree(b, 64);

    uint32_t* c = kcalloc(32, sizeof(uint32_t));
    if (!c) {
        return -1;
    }
    kfree(c, 32 * sizeof(uint32_t));

    uint8_t* d = kmalloc(32);
    if (!d) {
        return -1;
    }

    for (int i = 0; i < 32; i++) {
        d[i] = i;
    }

    uint8_t* d2 = krealloc(d, 128, 32);
    if (!d2) {
        return -1;
    }
    kfree(d2, 128);

    size_t big_size = PAGE_SIZE * 3 + 100;
    uint8_t* big = kmalloc(big_size);
    if (!big) {
        return -1;
    }
    kfree(big, big_size);

    log("alloc test: all tests passed\n", GREEN);
    return 0;
}
