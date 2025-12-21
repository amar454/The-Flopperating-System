#include "pmm.h"
#include "alloc.h"
#include "utils.h"
#include "../lib/logging.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
static spinlock_t heap_lock;
static box_t* boxes = NULL;
static int heap_initialized = 0;

static box_t* heap_create_box(void) {
    void* page = pmm_alloc_page();

    if (!page) {
        return NULL;
    }

    box_t* box = (box_t*) page;

    spinlock_init(&box->lock);

    uintptr_t base = (uintptr_t) page;

    box->page = page;
    box->total_blocks = BLOCKS_PER_BOX;
    box->map = (uint8_t*) (base + sizeof(box_t));
    box->data_pointer = (void*) (base + sizeof(box_t) + (BLOCKS_PER_BOX + 7) / 8);

    for (int i = 0; i < (BLOCKS_PER_BOX + 7) / 8; i++) {
        box->map[i] = 0;
    }

    spinlock(&heap_lock);
    box->next = boxes;
    boxes = box;
    spinlock_unlock(&heap_lock, true);

    return box;
}


static int heap_map_find_free(uint8_t* map, int total_blocks, int needed) {
    int run = 0;
    int start = -1;
    for (int i = 0; i < total_blocks; i++) {
        int byte = i / 8;
        int bit = i % 8;

        if (!(map[byte] & (1 << bit))) {
            if (run == 0) {
                start = i;
            }

            run++;

            if (run >= needed) {
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

    // set object
    heap_map_set(box->map, start, needed, true);

    uintptr_t mem = (uintptr_t) box->data_pointer + start * BLOCK_SIZE;

    object_t* obj = (object_t*) mem;
    obj->box = box;
    obj->size = size;

    spinlock_unlock(&box->lock, true);
    return (void*) (mem + OBJECT_ALIGN);
}


static int heap_fetch_block_index(box_t* box, void* mem_ptr) {
    uintptr_t base = (uintptr_t) box->data_pointer;
    uintptr_t p = (uintptr_t) mem_ptr;

    if (p < base) {
        return -1;
    }

    uintptr_t diff = p - base;

    if (diff % BLOCK_SIZE) {
        return -1;
    }

    int idx = diff / BLOCK_SIZE;

    if (idx < 0 || idx >= box->total_blocks) {
        return -1;
    }

    return idx;
}

void heap_init(void) {
    if (heap_initialized) {
        return;
    }

    boxes = NULL;

    // don't need to do much, just create a box
    if (!heap_create_box()) {
        return;
    }

    heap_initialized = 1;

    log("heap: init - ok\n", GREEN);
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

    // shouldn't happen
    if (!heap_initialized) {
        heap_init();
    }

    // search for boxes until a box with appropriate size free is found
    for (box_t* b = boxes; b; b = b->next) {
        void* r = heap_box_alloc(b, size);
        if (r) {
            return r;
        }
    }

    box_t* b = heap_create_box();
    if (!b) {
        return NULL;
    }

    return heap_box_alloc(b, size);
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
    int idx = heap_fetch_block_index(b, (void*) obj);

    if (idx < 0) {
        return;
    }

    heap_map_set(b->map, idx, needed, false);
}

void* kcalloc(size_t n, size_t s) {
    size_t t = n * s;
    void* p = kmalloc(t);

    if (!p) {
        return NULL;
    }

    flop_memset(p, 0, t);
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

    size_t copy = (old->size < new_size) ? old->size : new_size;
    flop_memcpy(n, ptr, copy);
    kfree(ptr, old_size);

    return n;
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
