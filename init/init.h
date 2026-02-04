#ifndef INIT_H
#define INIT_H
#include "../multiboot/multiboot.h"

typedef enum {
    INIT_STAGE_EARLY,
    INIT_STAGE_CPU,
    INIT_STAGE_BLOCK,
    INIT_STAGE_MEM,
    INIT_STAGE_MIDDLE,
    INIT_STAGE_FS,
    INIT_STAGE_TASK,
    INIT_STAGE_SYS,
    INIT_STAGE_COUNT
} init_stage_t;

typedef struct init_cfg {
    bool early;
    bool cpu;
    bool block;
    bool mem;
    bool middle;
    bool fs;
    bool task;
    bool sys;
} init_cfg_t;

extern init_cfg_t default_config;

void init(multiboot_info_t* mb_info, init_cfg_t config);

#endif // INIT_H
