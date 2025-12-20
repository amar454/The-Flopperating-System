#ifndef ALLOC_H
#define ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct box box_t;

typedef struct box {
    box_t* next;
    void* page;
    void* data_pointer;
    uint8_t* map;
    int total_blocks;
} box_t;

typedef struct {
    box_t* box;
    size_t size;
} object_t;

#define BLOCK_SIZE 32
#define OBJECT_ALIGN sizeof(void*)
#define BLOCKS_PER_BOX ((PAGE_SIZE - sizeof(box_t)) / (BLOCK_SIZE + 1))

void* kmalloc(size_t size);
void kfree(void* ptr, size_t size);
void* kcalloc(size_t n, size_t s);
void* krealloc(void* ptr, size_t new_size, size_t old_size);
int kmalloc_memtest(void);
void heap_init(void);
#endif
