/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

[DESCRIPTION] - heap allocator implementation. uses my custom "boxes" allocator, uses a hash table to store boxes.
*/

#include "pmm.h"
#include "vmm.h"
#include "alloc.h"
#include "utils.h"
#include "../lib/logging.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static spinlock_t heap_lock = SPINLOCK_INIT;
static box_t* boxes = NULL;
static int heap_initialized = 0;
static uint32_t next_box_id = 0;

static void box_hash_insert(uint32_t id, box_t* box);
static void box_hash_remove(uint32_t id);
static box_t* box_hash_lookup(uint32_t id);

static inline uint32_t box_hash_resolve_index(uint32_t id) {
    return (id * 2654435761u) & BOX_HASH_MASK;
}

static void box_hash_insert(uint32_t id, box_t* box) {
    uint32_t idx = box_hash_resolve_index(id);
    for (;;) {
        // found empty slot
        if (!box_hash[idx].value) {
            box_hash[idx].key = id;
            box_hash[idx].value = box;
            return;
        }

        // probe forward
        idx = (idx + 1) & BOX_HASH_MASK;
    }
}

static void box_hash_remove(uint32_t id) {
    uint32_t idx = box_hash_resolve_index(id);
    for (;;) {
        // if we hit and empty slot, its key is not present
        if (!box_hash[idx].value) {
            return;
        }

        // found key to delete
        if (box_hash[idx].key == id) {
            break;
        }

        // continue probing
        idx = (idx + 1) & BOX_HASH_MASK;
    }

    // remove entry
    box_hash[idx].value = NULL;

    // start at next slot
    idx = (idx + 1) & BOX_HASH_MASK;

    // rehash forward element until we hit and empty slot
    // preserves the probe chains so we can continue to use the hash table
    while (box_hash[idx].value) {
        uint32_t rekey = box_hash[idx].key;
        box_t* rebox = box_hash[idx].value;

        // clear slot for now
        box_hash[idx].value = NULL;

        // reinsert
        box_hash_insert(rekey, rebox);

        // continue probing
        idx = (idx + 1) & BOX_HASH_MASK;
    }
}

static void box_init(box_t* box, void* page) {
    uintptr_t base = (uintptr_t) page;

    spinlock_init(&box->lock);

    box->page = page;
    box->total_blocks = BLOCKS_PER_BOX;

    // place bitmap after the box data structure
    box->map = (uint8_t*) (base + sizeof(box_t));

    // data comes after bitmap
    box->data_pointer = (void*) (base + sizeof(box_t) + ((BLOCKS_PER_BOX + 7) / 8));

    flop_memset(box->map, 0, (BLOCKS_PER_BOX + 7) / 8);
}

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
    int run = 0, start = -1;

    // look through bitmap for a contiguous run
    for (int i = 0; i < total; i++) {
        int byte = i / 8;
        int bit = i % 8;

        // check if block is free
        if (!(map[byte] & (1 << bit))) {
            if (run == 0) {
                start = i;
            }

            // extend current free run
            if (++run >= needed) {
                return start;
            }
        } else {
            // reset run if block is used
            run = 0;
            start = -1;
        }
    }

    // no suitable run was found
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

    // look through bitmap for a contiguous run
    int start = heap_map_find_free(box->map, box->total_blocks, needed);

    if (start < 0) {
        spinlock_unlock(&box->lock, true);
        return NULL;
    }

    // mark blocks as used
    heap_map_set(box->map, start, needed, true);

    // get physical address inside data region
    uintptr_t mem = (uintptr_t) box->data_pointer + start * BLOCK_SIZE;
    object_t* obj = (object_t*) mem;

    // store obj data
    obj->box = box;
    obj->size = size;

    spinlock_unlock(&box->lock, true);

    return (void*) (mem + OBJECT_ALIGN);
}

static bool heap_box_is_empty(box_t* box) {
    // look through bitmap to make sure no blocks are used
    int bytes = (box->total_blocks + 7) / 8;
    for (int i = 0; i < bytes; i++) {
        if (box->map[i]) {
            return false;
        }
    }
    return true;
}

static void heap_box_free(box_t* box) {
    if (!box) {
        return;
    }

    spinlock(&box->lock);

    // if still in use, unlock and return
    if (!heap_box_is_empty(box)) {
        spinlock_unlock(&box->lock, true);
        return;
    }

    spinlock_unlock(&box->lock, true);
    spinlock(&heap_lock);

    // unlink box from list
    box_t** it = &boxes;
    while (*it) {
        if (*it == box) {
            *it = box->next;
            break;
        }
        it = &(*it)->next;
    }

    // remove box from hash table
    box_hash_remove(box->id);
    spinlock_unlock(&heap_lock, true);
    pmm_free_page(box->page);
}

static int heap_fetch_block_index(box_t* box, void* mem) {
    uintptr_t base = (uintptr_t) box->data_pointer;
    uintptr_t p = (uintptr_t) mem;

    // pointer must be within the data region
    if (p < base) {
        return -1;
    }

    uintptr_t diff = p - base;

    // pointer must be aligned to block size
    if (diff % BLOCK_SIZE) {
        return -1;
    }

    int idx = diff / BLOCK_SIZE;

    // make sure pointer is within box range
    return (idx >= 0 && idx < box->total_blocks) ? idx : -1;
}

void heap_init(void) {
    if (heap_initialized) {
        return;
    }

    spinlock_init(&heap_lock);

    boxes = NULL;

    // all we need to do is create a box
    if (!heap_create_box()) {
        return;
    }

    heap_initialized = 1;
    log("heap: init - ok\n", GREEN);
}

static void* heap_box_iterate(size_t size) {
    spinlock(&heap_lock);

    // iterate through all boxes
    BOX_LOOKUP {
        // attempt allocation
        void* r = heap_box_alloc(b, size);

        if (r) {
            spinlock_unlock(&heap_lock, true);
            return r;
        }
    }
    spinlock_unlock(&heap_lock, true);

    // no box had enough room
    return NULL;
}

static box_t* box_hash_lookup(uint32_t id) {
    uint32_t idx = box_hash_resolve_index(id);
    for (;;) {
        // if empty slot is reached, the key is not stored
        if (!box_hash[idx].value) {
            return NULL;
        }

        // found the matching key
        if (box_hash[idx].key == id) {
            return box_hash[idx].value;
        }

        // continue probing
        idx = (idx + 1) & BOX_HASH_MASK;
    }
}

// allocate a memory object of size
void* kmalloc(size_t size) {
    // zero size allocation returns NULL
    if (size == 0) {
        return NULL;
    }

    // if allocation is larger than a page, use page allocation
    if (size > PAGE_SIZE) {
        size_t pages = (size + OBJECT_ALIGN + PAGE_SIZE - 1) / PAGE_SIZE;
        void* mem = pmm_alloc_pages(0, pages);
        if (!mem) {
            return NULL;
        }
        object_t* obj = (object_t*) mem;

        // no box for large allocations
        obj->box = NULL;
        obj->size = size;
        return (void*) ((uintptr_t) mem + OBJECT_ALIGN);
    }

    // if we haven't initialized the heap
    // *which shouldn't happen*
    // initialize it
    if (!heap_initialized) {
        heap_init();
    }

    // try to allocate from existing boxes
    void* r = heap_box_iterate(size);
    if (r) {
        return r;
    }

    // if we couldn't allocate from existing boxes,
    // create a new box and allocate from it
    box_t* b = heap_create_box();
    if (!b) {
        return NULL;
    }

    return b ? heap_box_alloc(b, size) : NULL;
}

// free a memory object of size at ptr
void kfree(void* ptr, size_t size) {
    if (!ptr) {
        return;
    }

    // retrieve object metadata stored before the data pointer
    object_t* obj = (object_t*) ((uintptr_t) ptr - OBJECT_ALIGN);

    // if pointer wasn't in a heap box, free it's pages
    if (!obj->box) {
        size_t pages = (obj->size + OBJECT_ALIGN + PAGE_SIZE - 1) / PAGE_SIZE;
        pmm_free_pages((void*) obj, 0, pages);
        return;
    }

    // object is in a box
    box_t* b = obj->box;

    // determine how many blocks are in need of freeing
    int needed = (obj->size + OBJECT_ALIGN + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // fetch the bitmap block index
    int idx = heap_fetch_block_index(b, obj);
    if (idx >= 0) {
        // mark blocks as free
        heap_map_set(b->map, idx, needed, false);

        // if box is empty, free it
        if (heap_box_is_empty(b)) {
            heap_box_free(b);
        }
    }
}

// allocate memory object for an array
// set it to zero
void* kcalloc(size_t n, size_t s) {
    size_t t = n * s;
    void* p = kmalloc(t);
    if (p) {
        // set it to zero
        flop_memset(p, 0, t);
    }
    return p;
}

// reallocate memory object from old size to new size
void* krealloc(void* ptr, size_t new_size, size_t old_size) {
    // realloc(NULL) is basically a botched kmalloc()
    if (!ptr) {
        return kmalloc(new_size);
    }

    // realloc(ptr, 0) is basically a botched kfree()
    if (new_size == 0) {
        kfree(ptr, old_size);
        return NULL;
    }

    // fetch obj metadata
    object_t* old = (object_t*) ((uintptr_t) ptr - OBJECT_ALIGN);

    // allocate a new object
    void* n = kmalloc(new_size);
    if (!n) {
        return NULL;
    }

    // copy as much as possible
    size_t copy = old->size < new_size ? old->size : new_size;
    flop_memcpy(n, ptr, copy);

    // free old object
    kfree(ptr, old_size);
    return n;
}

void* kmalloc_guarded(size_t size) {
    if (size == 0) {
        return NULL;
    }

    // number of pages plus guard page
    size_t data_pages = (size + sizeof(guarded_object_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t total_pages = data_pages + 1;

    // allocate virtually contiguous pages
    void* base = pmm_alloc_pages(0, total_pages);
    if (!base) {
        return NULL;
    }

    // fetch addr of guard page
    uintptr_t guard_va = (uintptr_t) base + data_pages * PAGE_SIZE;

    // remove mapping so access faults
    vmm_unmap(vmm_get_current(), guard_va);

    // store object metadata at start
    guarded_object_t* obj = (guarded_object_t*) base;
    obj->size = size;
    obj->pages = total_pages;

    // return pointer to data
    return (void*) ((uintptr_t) base + sizeof(guarded_object_t));
}

void kfree_guarded(void* ptr) {
    if (!ptr) {
        return;
    }

    // fetch object metadata
    // and free allocation including the guard
    guarded_object_t* obj = (guarded_object_t*) ((uintptr_t) ptr - sizeof(guarded_object_t));
    pmm_free_pages((void*) obj, 0, obj->pages);
}

// test heap allocator with a variety of sizes.
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
