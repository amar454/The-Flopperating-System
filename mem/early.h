#ifndef EARLY_MEM_H
#define EARLY_MEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../multiboot/multiboot.h"
#define EARLY_PAGES_TOTAL 10
#define EARLY_META_PAGE 0
#define EARLY_POOL_PAGES 9
#define EARLY_BLOCK_SIZE 256
#define PAGE_SIZE 4096
#define EARLY_BIT_SET(i) (early.bitmap[(i) >> 3] |= (1u << ((i) & 7)))
#define EARLY_BIT_CLEAR(i) (early.bitmap[(i) >> 3] &= ~(1u << ((i) & 7)))
#define EARLY_BIT_TEST(i) (early.bitmap[(i) >> 3] & (1u << ((i) & 7)))

struct early_info {
    uint8_t* pool_base;
    uint8_t bitmap[64];
    uint32_t blocks_total;
    uint32_t blocks_free;
    bool initialized;
};

void early_allocator_init(void);
void* early_alloc(size_t size);
void early_free(void* ptr);
void early_allocator_destroy(void);
void early_bootstrap(multiboot_info_t* mb);

#endif /* EARLY_MEM_H */
