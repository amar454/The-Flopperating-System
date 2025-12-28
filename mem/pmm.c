/*

Copyright 2024, 2025 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later veregion_startion.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

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
#include <stdint.h>

struct buddy_allocator buddy;

// split a page into two lower order pages
static void pmm_buddy_split(uintptr_t addr, uint32_t order) {
    if (order == 0) {
        log("pmm_buddy_split: order=0, nothing to split\n", YELLOW);
        return;
    }

    uintptr_t half_size = ((uintptr_t) 1 << (order - 1)) * PAGE_SIZE;
    uintptr_t buddy_addr = addr + half_size;

    struct page* page_one = phys_to_page_index(addr);
    struct page* page_two = phys_to_page_index(buddy_addr);

    if (!page_one || !page_two) {
        log("pmm_buddy_split: invalid page(s)\n", RED);
        return;
    }

    page_one->order = order - 1;
    page_two->order = order - 1;
    page_one->is_free = 1;
    page_two->is_free = 1;

    page_one->next = buddy.free_list[order - 1];
    buddy.free_list[order - 1] = page_one;

    page_two->next = buddy.free_list[order - 1];
    buddy.free_list[order - 1] = page_two;
}

// merge two pages into a higher order page
static void pmm_buddy_merge(uintptr_t addr, uint32_t order) {
    uintptr_t buddy_addr = addr ^ (((uintptr_t) 1 << order) * PAGE_SIZE);

    struct page* page = phys_to_page_index(addr);
    struct page* buddy_page = phys_to_page_index(buddy_addr);

    if (!page) {
        log("pmm_buddy_merge: invalid page\n", RED);
        return;
    }

    if (buddy_page && buddy_page->is_free && buddy_page->order == order) {
        // unlink buddy_page from its free list
        struct page** prev = &buddy.free_list[order];
        while (*prev && *prev != buddy_page) {
            prev = &(*prev)->next;
        }
        if (*prev == buddy_page) {
            *prev = buddy_page->next;
        }

        uintptr_t merged_addr = (addr < buddy_addr) ? addr : buddy_addr;

        pmm_buddy_merge(merged_addr, order + 1);
    } else {
        // can't merge, put this block into its list
        page->order = order;
        page->is_free = 1;
        page->next = buddy.free_list[order];
        buddy.free_list[order] = page;
    }
}

static inline uintptr_t align_up(uintptr_t x, uintptr_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static uintptr_t pmm_kernel_end(void) {
    extern char _kernel_end; // from linker
    return (uintptr_t) &_kernel_end;
}

static uintptr_t pmm_reserved_top(multiboot_info_t* mb) {
    uintptr_t top = pmm_kernel_end();

    if (mb) {
        uintptr_t a = (uintptr_t) mb;
        if (a > top) {
            top = a;
        }
    }

    if (PMM_HAS_MMAP(mb)) {
        uintptr_t t = (uintptr_t) mb->mmap_addr + (uintptr_t) mb->mmap_length;
        if (t > top) {
            top = t;
        }
    }

    if (PMM_HAS_MODS(mb)) {
        multiboot_module_t* m = (multiboot_module_t*) (uintptr_t) mb->mods_addr;
        for (uint32_t i = 0; i < mb->mods_count; i++) {
            uintptr_t end = (uintptr_t) m[i].mod_end;
            if (end > top) {
                top = end;
            }
        }
    }

    return PMM_ALIGN(top);
}

static uintptr_t pmm_find_page_info_placement(multiboot_info_t* mb, uintptr_t reserved_top, size_t bytes) {
    if (!PMM_HAS_MMAP(mb)) {
        return 0;
    }

    uintptr_t need = PMM_ALIGN(bytes);
    uint8_t* page = PMM_MMAP_BEGIN(mb);
    uint8_t* entry = PMM_MMAP_END(mb);

    while (page < entry) {
        multiboot_memory_map_t* mm = PMM_MMAP_ENTRY(page);
        if (!PMM_MMAP_ENTRY_VALID(mm)) {
            break;
        }

        if (PMM_REGION_USABLE(mm)) {
            uintptr_t region_start = PMM_REGION_START(mm);
            uintptr_t region_end = PMM_REGION_END(mm);
            uintptr_t start = PMM_ALIGN(reserved_top > region_start ? reserved_top : region_start);
            if (start < region_end && (region_end - start) >= need) {
                return start;
            }
        }

        page = PMM_MMAP_NEXT(mm);
    }
    return 0;
}

static void pmm_add_free(struct page* page, uintptr_t addr) {
    page->address = addr;
    page->order = 0;
    page->is_free = 1;
    page->next = buddy.free_list[0];
    buddy.free_list[0] = page;
}

static bool pmm_addr_in_pageinfo(uintptr_t addr, uintptr_t s, uintptr_t entry) {
    return addr >= s && addr < entry;
}

static bool pmm_skip_addr(uintptr_t addr) {
    return addr < buddy.memory_base || addr >= buddy.memory_end;
}

static size_t pmm_process_region(multiboot_memory_map_t* mm, uintptr_t s, uintptr_t entry) {
    size_t added = 0;
    uintptr_t region_start = PMM_REGION_START(mm);
    uintptr_t region_end = PMM_REGION_END(mm);

    for (uintptr_t a = region_start; a < region_end; a += PAGE_SIZE) {
        if (pmm_addr_in_pageinfo(a, s, entry)) {
            continue;
        }
        if (pmm_skip_addr(a)) {
            continue;
        }

        uint32_t idx = (uint32_t) ((a - buddy.memory_base) / PAGE_SIZE);
        if (idx >= buddy.total_pages) {
            continue;
        }

        pmm_add_free(&buddy.page_info[idx], a);
        added++;
    }

    return added;
}

void pmm_create_free_list(multiboot_info_t* mb) {
    if (!PMM_HAS_MMAP(mb)) {
        return;
    }

    uintptr_t page_info_slot = (uintptr_t) buddy.page_info;
    uintptr_t page_info_entry = page_info_slot + PMM_ALIGN(buddy.total_pages * sizeof(struct page));

    uint8_t* page = PMM_MMAP_BEGIN(mb);
    uint8_t* end = PMM_MMAP_END(mb);

    size_t added = 0;

    while (page < end) {
        multiboot_memory_map_t* mm = PMM_MMAP_ENTRY(page);
        if (!PMM_MMAP_ENTRY_VALID(mm)) {
            break;
        }

        if (PMM_REGION_USABLE(mm)) {
            added += pmm_process_region(mm, page_info_slot, page_info_entry);
        }

        page = PMM_MMAP_NEXT(mm);
    }
}

uint64_t pmm_count_usable_pages(multiboot_info_t* mb, uintptr_t* out_region_start_usable, uint64_t* out_total_bytes) {
    if (!PMM_HAS_MMAP(mb)) {
        if (out_region_start_usable) {
            *out_region_start_usable = 0;
        }
        if (out_total_bytes) {
            *out_total_bytes = 0;
        }
        return 0;
    }

    uint64_t total_bytes = 0;
    uintptr_t region_start_usable = 0;

    uint8_t* ptr = PMM_MMAP_BEGIN(mb);
    uint8_t* end = PMM_MMAP_END(mb);

    while (ptr < end) {
        multiboot_memory_map_t* mm = PMM_MMAP_ENTRY(ptr);
        if (!PMM_MMAP_ENTRY_VALID(mm)) {
            break;
        }

        uintptr_t region_start = PMM_REGION_START(mm);
        uintptr_t region_end = PMM_REGION_END(mm);

        if (PMM_REGION_USABLE(mm)) {
            log_address("pmm: region start (usable): ", region_start);
            log_address("pmm: region end (usable): ", region_end);
            echo("----------------------------------------\n", WHITE);
            if (region_end > region_start) {
                uint64_t region_bytes = (uint64_t) (region_end - region_start);
                total_bytes += region_bytes;

                if (region_start_usable == 0) {
                    region_start_usable = region_start;
                }
            }
        } else {
            log_address("pmm: region start (reserved): ", region_start);
            log_address("pmm: region end (reserved): ", region_end);
            echo("----------------------------------------\n", WHITE);
        }

        ptr = PMM_MMAP_NEXT(mm);
    }

    if (out_region_start_usable) {
        *out_region_start_usable = region_start_usable;
    }

    if (out_total_bytes) {
        *out_total_bytes = total_bytes;
    }

    return total_bytes / PAGE_SIZE;
}

static void
pmm_buddy_init(uint64_t usable_pages, uintptr_t memory_base_region_start_usable, multiboot_info_t* mb_info) {
    log("buddy: setting up page info array\n", GREEN);

    buddy.total_pages = usable_pages;
    buddy.memory_base = memory_base_region_start_usable;

    size_t page_info_bytes = buddy.total_pages * sizeof(struct page);
    uintptr_t reserved_top = pmm_reserved_top(mb_info);

    uintptr_t page_info_addr = pmm_find_page_info_placement(mb_info, reserved_top, page_info_bytes);
    if (!page_info_addr) {
        log("buddy: warning - could not find available region for page_info; using reserved_top fallback\n", YELLOW);
        page_info_addr = reserved_top;
    }

    buddy.page_info = (struct page*) page_info_addr;
    size_t page_info_pages = (page_info_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    buddy.memory_start = page_info_addr + page_info_pages * PAGE_SIZE;
    buddy.memory_end = buddy.memory_base + buddy.total_pages * PAGE_SIZE;

    log_uint("buddy: total pages: ", buddy.total_pages);
    log_uint("buddy: page_info size (pages): ", page_info_pages);
    log_address("buddy: memory_base: ", buddy.memory_base);
    log_address("buddy: page_info: ", (uintptr_t) buddy.page_info);
    log_address("buddy: memory_start: ", buddy.memory_start);
    log_address("buddy: memory_end: ", buddy.memory_end);

    // build the free list from the multiboot map
    pmm_create_free_list(mb_info);

    log("buddy: init - ok\n", GREEN);
}

void pmm_init(multiboot_info_t* mb_info) {
    if (!mb_info || !(mb_info->flags & MULTIBOOT_INFO_MEM_MAP)) {
        log("pmm: Invalid or missing Multiboot memory map\n", RED);
        return;
    }

    uintptr_t usable_start = 0;
    uint64_t total_memory_bytes = 0;
    uint64_t usable_pages = pmm_count_usable_pages(mb_info, &usable_start, &total_memory_bytes);

    if (usable_pages == 0 || usable_start == 0) {
        log("pmm: no usable pages found\n", RED);
        return;
    }

    log_uint("pmm: usable pages: ", usable_pages);
    log_uint("pmm: total memory bytes (from mmap): ", (uint32_t) (total_memory_bytes & 0xFFFFFFFFU));
    log_address("pmm: firegion_startt usable addr: ", usable_start);

    pmm_buddy_init(usable_pages, usable_start, mb_info);

    static spinlock_t buddy_lock_initializer = SPINLOCK_INIT;
    buddy.lock = buddy_lock_initializer;
    spinlock_init(&buddy.lock);

    // alloc test
    void* test_page = pmm_alloc_page();
    if (test_page != NULL) {
        log_address("pmm: test page: ", (uintptr_t) test_page);
        uint32_t* test_ptr = (uint32_t*) test_page;
        for (int i = 0; i < PAGE_SIZE / sizeof(uint32_t); i++) {
            test_ptr[i] = 0xDEADBEEF;
        }
        for (int i = 0; i < PAGE_SIZE / sizeof(uint32_t); i++) {
            if (test_ptr[i] != 0xDEADBEEF) {
                log("pmm: test page verification failed\n", RED);
                pmm_free_page(test_page);
                return;
            }
        }
        log("pmm: test page verification passed\n", GREEN);
        pmm_free_page(test_page);
    }

    log("pmm: init - ok\n", GREEN);
}

void pmm_copy_page(void* dst, void* src) {
    spinlock(&buddy.lock);
    for (int i = 0; i < PAGE_SIZE / sizeof(uint32_t); i++) {
        ((uint32_t*) dst)[i] = ((uint32_t*) src)[i];
    }
    spinlock_unlock(&buddy.lock, true);
}

// fetch a free block of at least requested order
static struct page* pmm_fetch_order_block(uint32_t order) {
    // iterate from order up to the maximum order
    for (uint32_t j = order; j <= MAX_ORDER; j++) {
        // if free block of order is free
        if (buddy.free_list[j]) {
            // fetch the first block from the free list
            struct page* block = buddy.free_list[j];
            // set to free list head
            buddy.free_list[j] = block->next;

            return block;
        }
    }
    // No suitable block found
    return NULL;
}

// split a block down to order
static void pmm_determine_split(struct page* block, uint32_t from_order, uint32_t to_order) {
    //  split until the current block is larger than desired
    while (from_order > to_order) {
        from_order--;

        // get size of the block
        uintptr_t split_size = ((uintptr_t) 1 << from_order) * PAGE_SIZE;
        // fetch the physical address of the buddy
        uintptr_t buddy_addr = block->address + split_size;

        // fetch the buddy page struct
        struct page* right = phys_to_page_index(buddy_addr);

        // if it doesn't exist stop splitting
        if (!right) {
            return;
        }

        // set buddy info
        right->address = buddy_addr;
        right->order = from_order;
        right->is_free = 1;

        // add buddy to free list of its order
        right->next = buddy.free_list[from_order];
        buddy.free_list[from_order] = right;

        // update order of original block
        block->order = from_order;
    }
}

// allocate a block of the order
static void* pmm_alloc_block(uint32_t order) {
    // fetch page struct of order block
    struct page* block = pmm_fetch_order_block(order);
    if (!block) {
        return NULL;
    }

    // mark block used
    block->is_free = 0;
    block->order = order;

    // split to order if needed
    pmm_determine_split(block, block->order, order);

    return (void*) block->address;
}

// free previously allocated block
static void pmm_free_block(uintptr_t addr, uint32_t order) {
    // fetch page struct of block
    struct page* page = phys_to_page_index(addr);

    if (!page) {
        return;
    }

    // mark the block free
    page->is_free = 1;

    // Attempt to merge with its buddy to coalesce free space
    pmm_buddy_merge(page->address, order);
}

// allocate count pages of order
void* pmm_alloc_pages(uint32_t order, uint32_t count) {
    if (order > MAX_ORDER || count == 0) {
        return NULL;
    }

    spinlock(&buddy.lock);

    void* start_page = NULL;
    // allocate 'count' blocks of 'order' pages each
    for (uint32_t i = 0; i < count; i++) {
        void* pg = pmm_alloc_block(order);

        if (!pg) {
            // rollback already-allocated pages
            if (start_page) {
                pmm_free_pages(start_page, order, i);
            }
            log("pmm: Out of memory!\n", RED);
            spinlock_unlock(&buddy.lock, true);
            return NULL;
        }

        if (!start_page) {
            start_page = pg;
        }
    }

    spinlock_unlock(&buddy.lock, true);
    return start_page;
}

// free count pages of order at address
void pmm_free_pages(void* addr, uint32_t order, uint32_t count) {
    if (!addr || order > MAX_ORDER || count == 0) {
        return;
    }
    spinlock(&buddy.lock);

    uintptr_t cur = (uintptr_t) addr;

    // iterate through pages and free each block
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_block(cur, order);
        cur += (1u << order) * PAGE_SIZE;
    }

    spinlock_unlock(&buddy.lock, true);
}

void* pmm_alloc_page(void) {
    return pmm_alloc_pages(0, 1);
}

void pmm_free_page(void* addr) {
    pmm_free_pages(addr, 0, 1);
}

uint32_t pmm_get_memory_size(void) {
    return buddy.total_pages * PAGE_SIZE;
}

uint32_t pmm_get_page_count() {
    return buddy.total_pages;
}

uint32_t pmm_get_free_memory_size(void) {
    int free_pages = 0;
    for (int i = 0; i <= MAX_ORDER; i++) {
        struct page* page = buddy.free_list[i];
        while (page) {
            free_pages++;
            page = page->next;
        }
    }
    return free_pages * PAGE_SIZE;
}

struct page* pmm_get_last_used_page(void) {
    for (int page_index = buddy.total_pages - 1; page_index >= 0; page_index--) {
        struct page* page = &buddy.page_info[page_index];
        if (!page->is_free) {
            return page;
        }
    }
    return NULL;
}

uintptr_t page_to_phys_addr(struct page* page) {
    return page->address;
}

// index relative to the memory_base anchor used when building page_info
uint32_t page_index(uintptr_t addr) {
    return (addr - buddy.memory_base) / PAGE_SIZE;
}

struct page* phys_to_page_index(uintptr_t addr) {
    // check if
    if (addr < buddy.memory_base || addr >= buddy.memory_end) {
        return NULL;
    }
    uint32_t index = page_index(addr);
    if (index >= buddy.total_pages) {
        return NULL;
    }
    return &buddy.page_info[index];
}

int pmm_is_valid_addr(uintptr_t addr) {
    if (addr % PAGE_SIZE != 0) {
        return 0;
    }
    if (addr < buddy.memory_base || addr >= buddy.memory_end) {
        return 0;
    }
    uint32_t index = page_index(addr);
    if (index >= buddy.total_pages) {
        return 0;
    }
    return 1;
}

static size_t cm_next_pow2(size_t x) {
    if (x <= 4) {
        return 4;
    }
    x--;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) {
        x |= x >> shift;
    }
    return x + 1;
}

static uint32_t cm_hash(uint8_t k) {
    uint32_t x = k;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x ? x : 1;
}

static int cm_init(child_map_t* m, size_t cap) {
    size_t c = cm_next_pow2(cap);
    m->keys = (uint8_t*) kmalloc(c);
    m->vals = (void**) kmalloc(sizeof(void*) * c);
    m->state = (uint8_t*) kmalloc(c);
    if (!m->keys || !m->vals || !m->state) {
        if (m->keys) {
            kfree(m->keys, c);
        }
        if (m->vals) {
            kfree(m->vals, sizeof(void*) * c);
        }
        if (m->state) {
            kfree(m->state, c);
        }
        return -1;
    }
    flop_memset(m->state, 0, c);
    m->cap = c;
    m->len = 0;
    return 0;
}

static void cm_free(child_map_t* m) {
    if (!m) {
        return;
    }
    if (m->keys) {
        kfree(m->keys, m->cap);
    }
    if (m->vals) {
        kfree(m->vals, sizeof(void*) * m->cap);
    }
    if (m->state) {
        kfree(m->state, m->cap);
    }
    m->keys = NULL;
    m->vals = NULL;
    m->state = NULL;
    m->cap = 0;
    m->len = 0;
}

static int cm_resize(child_map_t* m, size_t newcap) {
    child_map_t n;
    if (cm_init(&n, newcap)) {
        return -1;
    }
    for (size_t i = 0; i < m->cap; i++) {
        if (m->state[i] == 1) {
            uint8_t k = m->keys[i];
            void* v = m->vals[i];
            size_t mask = n.cap - 1;
            size_t pos = cm_hash(k) & mask;
            while (n.state[pos] == 1) {
                pos = (pos + 1) & mask;
            }
            n.state[pos] = 1;
            n.keys[pos] = k;
            n.vals[pos] = v;
            n.len++;
        }
    }
    cm_free(m);
    *m = n;
    return 0;
}

static void** cm_get_ref(child_map_t* m, uint8_t key) {
    if (!m->cap) {
        if (cm_init(m, 4)) {
            return NULL;
        }
    }
    size_t mask = m->cap - 1;
    size_t pos = cm_hash(key) & mask;
    size_t firegion_startt_del = (size_t) -1;
    for (;;) {
        uint8_t st = m->state[pos];
        if (st == 0) {
            size_t use = (firegion_startt_del != (size_t) -1) ? firegion_startt_del : pos;
            m->state[use] = 1;
            m->keys[use] = key;
            m->vals[use] = NULL;
            m->len++;
            if (m->len * 10 > m->cap * 7) {
                if (cm_resize(m, m->cap << 1)) {
                    return NULL;
                }
                return cm_get_ref(m, key);
            }
            return &m->vals[use];
        } else if (st == 2) {
            if (firegion_startt_del == (size_t) -1) {
                firegion_startt_del = pos;
            }
        } else {
            if (m->keys[pos] == key) {
                return &m->vals[pos];
            }
        }
        pos = (pos + 1) & mask;
    }
}

static void* cm_find(child_map_t* m, uint8_t key) {
    if (!m->cap) {
        return NULL;
    }
    size_t mask = m->cap - 1;
    size_t pos = cm_hash(key) & mask;
    for (;;) {
        uint8_t st = m->state[pos];
        if (st == 0) {
            return NULL;
        }
        if (st == 1 && m->keys[pos] == key) {
            return m->vals[pos];
        }
        pos = (pos + 1) & mask;
    }
}

static int cm_del(child_map_t* m, uint8_t key) {
    if (!m->cap) {
        return -1;
    }
    size_t mask = m->cap - 1;
    size_t pos = cm_hash(key) & mask;
    for (;;) {
        uint8_t st = m->state[pos];
        if (st == 0) {
            return -1;
        }
        if (st == 1 && m->keys[pos] == key) {
            m->state[pos] = 2;
            m->vals[pos] = NULL;
            m->len--;
            return 0;
        }
        pos = (pos + 1) & mask;
    }
}

static radix_node_t* rt_new_node(void) {
    radix_node_t* n = (radix_node_t*) kmalloc(sizeof(radix_node_t));
    if (!n) {
        return NULL;
    }
    n->entry = NULL;
    n->map.cap = 0;
    n->map.len = 0;
    n->map.keys = NULL;
    n->map.vals = NULL;
    n->map.state = NULL;
    return n;
}

static void rt_free_node_recuregion_startive(radix_node_t* n) {
    if (!n) {
        return;
    }
    if (n->map.cap) {
        for (size_t i = 0; i < n->map.cap; i++) {
            if (n->map.state[i] == 1) {
                radix_node_t* child = (radix_node_t*) n->map.vals[i];
                rt_free_node_recuregion_startive(child);
            }
        }
    }
    if (n->entry) {
        pmm_free_page((void*) n->entry->phys);
        kfree(n->entry, sizeof(page_cache_entry_t));
    }
    cm_free(&n->map);
    kfree(n, sizeof(radix_node_t));
}

static inline uint8_t rt_key_part(uint64_t k, int level) {
    return (uint8_t) ((k >> (level * 8)) & 0xFF);
}

static int radix_init(radix_tree_t** out) {
    if (!out) {
        return -1;
    }
    radix_tree_t* t = (radix_tree_t*) kmalloc(sizeof(radix_tree_t));
    if (!t) {
        return -1;
    }
    t->root = NULL;
    *out = t;
    return 0;
}

static void radix_free(radix_tree_t* t) {
    if (!t) {
        return;
    }
    if (t->root) {
        rt_free_node_recuregion_startive(t->root);
    }
    kfree(t, sizeof(radix_tree_t));
}

static page_cache_entry_t* radix_get_entry(radix_tree_t* t, uint64_t key) {
    if (!t || !t->root) {
        return NULL;
    }
    radix_node_t* n = t->root;
    for (int level = 7; level > 0; --level) {
        void* child = cm_find(&n->map, rt_key_part(key, level));
        if (!child) {
            return NULL;
        }
        n = (radix_node_t*) child;
    }
    return n->entry;
}

static int radix_set_entry(radix_tree_t* t, uint64_t key, page_cache_entry_t* entry) {
    if (!t) {
        return -1;
    }
    if (!t->root) {
        t->root = rt_new_node();
        if (!t->root) {
            return -1;
        }
    }
    radix_node_t* n = t->root;
    for (int level = 7; level > 0; --level) {
        uint8_t part = rt_key_part(key, level);
        void** slot = cm_get_ref(&n->map, part);
        if (!slot) {
            return -1;
        }
        if (!*slot) {
            radix_node_t* nn = rt_new_node();
            if (!nn) {
                return -1;
            }
            *slot = nn;
        }
        n = (radix_node_t*) (*slot);
    }
    if (n->entry) {
        pmm_free_page((void*) n->entry->phys);
        kfree(n->entry, sizeof(page_cache_entry_t));
    }
    n->entry = entry;
    return 0;
}

static void radix_del_entry(radix_tree_t* t, uint64_t key) {
    if (!t || !t->root) {
        return;
    }
    radix_node_t* stack[9];
    uint8_t part_stack[9];
    radix_node_t* n = t->root;
    int depth = 0;
    stack[depth] = n;
    for (int level = 7; level > 0; --level) {
        uint8_t part = rt_key_part(key, level);
        void* child = cm_find(&n->map, part);

        if (!child) {
            return;
        }

        n = (radix_node_t*) child;
        stack[++depth] = n;
        part_stack[depth] = part;
    }
    if (!n->entry) {
        return;
    }
    pmm_free_page((void*) n->entry->phys);
    kfree(n->entry, sizeof(page_cache_entry_t));
    n->entry = NULL;
    for (int i = depth; i > 0; --i) {
        radix_node_t* cur = stack[i];
        if (cur->entry) {
            break;
        }

        if (cur->map.len == 0) {
            radix_node_t* parent = stack[i - 1];
            cm_del(&parent->map, part_stack[i]);
            cm_free(&cur->map);
            kfree(cur, sizeof(radix_node_t));
        } else {
            break;
        }
    }
    if (t->root && t->root->entry == NULL && t->root->map.len == 0) {
        cm_free(&t->root->map);
        kfree(t->root, sizeof(radix_node_t));
        t->root = NULL;
    }
}

page_cache_t page_cache;

void page_cache_init(void) {
    page_cache.tree = NULL;
    radix_init(&page_cache.tree);
    page_cache.lru_head = page_cache.lru_tail = NULL;
    page_cache.page_count = 0;
    static spinlock_t initializer = SPINLOCK_INIT;
    page_cache.lock = initializer;
    spinlock_init(&page_cache.lock);
}

static void _lru_remove(page_cache_entry_t* entry) {
    if (!entry) {
        return;
    }
    if (entry->prev_lru) {
        entry->prev_lru->next_lru = entry->next_lru;
    }
    if (entry->next_lru) {
        entry->next_lru->prev_lru = entry->prev_lru;
    }
    if (page_cache.lru_head == entry) {
        page_cache.lru_head = entry->next_lru;
    }
    if (page_cache.lru_tail == entry) {
        page_cache.lru_tail = entry->prev_lru;
    }
    entry->prev_lru = entry->next_lru = NULL;
}

static void _lru_add_head(page_cache_entry_t* entry) {
    entry->prev_lru = NULL;
    entry->next_lru = page_cache.lru_head;
    if (page_cache.lru_head) {
        page_cache.lru_head->prev_lru = entry;
    }
    page_cache.lru_head = entry;
    if (!page_cache.lru_tail) {
        page_cache.lru_tail = entry;
    }
}

void* page_cache_get(uint64_t idx) {
    spinlock(&page_cache.lock);
    page_cache_entry_t* entry = radix_get_entry(page_cache.tree, idx);
    if (entry) {
        entry->refcount++;
        _lru_remove(entry);
        _lru_add_head(entry);
        spinlock_unlock(&page_cache.lock, true);
        return (void*) entry->phys;
    }
    void* page = pmm_alloc_page();
    if (!page) {
        spinlock_unlock(&page_cache.lock, true);
        return NULL;
    }
    entry = (page_cache_entry_t*) kmalloc(sizeof(page_cache_entry_t));
    entry->phys = (uintptr_t) page;
    entry->idx = idx;
    entry->prev_lru = entry->next_lru = NULL;
    entry->dirty = false;
    entry->refcount = 1;
    if (radix_set_entry(page_cache.tree, idx, entry) < 0) {
        pmm_free_page(page);
        kfree(entry, sizeof(page_cache_entry_t));
        spinlock_unlock(&page_cache.lock, true);
        return NULL;
    }
    _lru_add_head(entry);
    page_cache.page_count++;
    spinlock_unlock(&page_cache.lock, true);
    return page;
}

void page_cache_mark_dirty(uint64_t idx) {
    spinlock(&page_cache.lock);
    page_cache_entry_t* entry = radix_get_entry(page_cache.tree, idx);
    if (entry) {
        entry->dirty = true;
    }
    spinlock_unlock(&page_cache.lock, true);
}

void page_cache_release(uint64_t idx) {
    spinlock(&page_cache.lock);
    page_cache_entry_t* entry = radix_get_entry(page_cache.tree, idx);
    if (entry) {
        if (entry->refcount > 0) {
            entry->refcount--;
        }
    }
    spinlock_unlock(&page_cache.lock, true);
}

/* evict one least-recently-used page if any. */
int page_cache_evict_one(void) {
    spinlock(&page_cache.lock);
    page_cache_entry_t* victim = page_cache.lru_tail;
    if (!victim) {
        spinlock_unlock(&page_cache.lock, true);
        return 0;
    }
    if (victim->refcount > 0) {
        spinlock_unlock(&page_cache.lock, true);
        return 0;
    }
    _lru_remove(victim);
    radix_del_entry(page_cache.tree, victim->idx);
    page_cache.page_count--;
    spinlock_unlock(&page_cache.lock, true);
    return 1;
}

void page_cache_remove(uint64_t idx) {
    spinlock(&page_cache.lock);
    page_cache_entry_t* entry = radix_get_entry(page_cache.tree, idx);
    if (!entry) {
        spinlock_unlock(&page_cache.lock, true);
        return;
    }
    if (entry->refcount > 0) {
        spinlock_unlock(&page_cache.lock, true);
        return;
    }
    _lru_remove(entry);
    radix_del_entry(page_cache.tree, idx);
    page_cache.page_count--;
    spinlock_unlock(&page_cache.lock, true);
}

void page_cache_free_all(void) {
    spinlock(&page_cache.lock);
    if (page_cache.tree) {
        radix_free(page_cache.tree);
        page_cache.tree = NULL;
    }
    page_cache.lru_head = page_cache.lru_tail = NULL;
    page_cache.page_count = 0;
    spinlock_unlock(&page_cache.lock, true);
}

void log_page_info(struct page* page) {
    log_address("pmm: page address: ", page->address);
    log_uint("pmm: page order: ", page->order);
    log_uint("pmm: page is_free: ", page->is_free);
    log_address("pmm: page next: ", (uintptr_t) page->next);
}

void print_mem_info(void) {
    log("Memory Info:\n", LIGHT_GRAY);
    log("Total pages: ", LIGHT_GRAY);
    log_uint("", buddy.total_pages);
    log("\nFree pages: ", LIGHT_GRAY);
    for (int i = 0; i <= MAX_ORDER; i++) {
        log_uint("", (uint32_t) buddy.free_list[i]);
        log(" ", LIGHT_GRAY);
    }
    log("\n", LIGHT_GRAY);
}
