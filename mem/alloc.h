#ifndef ALLOC_H
#define ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct box box_t;
#include "../task/sync/spinlock.h"

typedef struct box {
    box_t* next;
    void* page;
    void* data_pointer;
    uint8_t* map;
    uint16_t total_blocks;
    spinlock_t lock;
    uint32_t id;
} box_t;

#define BOX_HASH_SIZE 256
#define BOX_HASH_MASK (BOX_HASH_SIZE - 1)

typedef struct {
    uint32_t key;
    box_t* value;
} box_hash_entry_t;

static box_hash_entry_t box_hash[BOX_HASH_SIZE];

typedef struct {
    box_t* box;
    size_t size;
} object_t;

typedef struct guarded_object {
    size_t size;
    size_t pages;
} guarded_object_t;

#define BLOCK_SIZE 32
#define OBJECT_ALIGN sizeof(void*)
#define BLOCKS_PER_BOX ((PAGE_SIZE - sizeof(box_t)) / (BLOCK_SIZE + 1))
#define BOX_LOOKUP for (box_t* b = boxes; b; b = b->next)

void* kmalloc(size_t size);
void kfree(void* ptr, size_t size);
void* kcalloc(size_t n, size_t s);
void* krealloc(void* ptr, size_t new_size, size_t old_size);
int kmalloc_memtest(void);
void heap_init(void);
#endif
