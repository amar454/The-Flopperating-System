/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
#include "../task/sched.h"
#include "../drivers/io/io.h"
#include "../drivers/ata/ata.h"
#include "../drivers/vga/vgahandler.h"
#include "../drivers/vga/framebuffer.h"
#include "../lib/logging.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../mem/vmm.h"
#include "../mem/pmm.h"
#include "../mem/early.h"
#include "../mem/gdt.h"
#include "../mem/paging.h"
#include "../fs/tmpflopfs/tmpflopfs.h"
#include "../mem/utils.h"
#include "../kernel/kernel.h"
#include "../multiboot/multiboot.h"
extern void framebuffer_term_init();

void init_early(multiboot_info_t* mb_info) {
    framebuffer_init(mb_info);
    framebuffer_term_init();
    log("floppaOS kernel framebuffer: init - ok\n", GREEN);
    log("floppaOS - The Floperrating system, a free and open-source 32-bit hobby operating system\n", YELLOW);
    sleep_seconds(1);
    log("Kernel compilation time: " __DATE__ " at " __TIME__ "\n", YELLOW);
    log("License: GPLv3\n", YELLOW);
    log("Date created: October 2024\n", YELLOW);
    log("Author: Amar Djulovic <aaamargml@gmail.com>\n", YELLOW);
    log("Kernel version: " VERSION "\n", YELLOW);
    log("Starting floppaOS kernel...\n", YELLOW);

    early_allocator_init();
    early_bootstrap(mb_info);
    // to be destroyed in init_mem()
}

void init_cpu(void) {
    gdt_init();
    sleep_seconds(1);
    interrupts_init();
}

void init_block() {
    ata_init();
    sleep_seconds(1);
}

void init_mem(multiboot_info_t* mb_info) {
    early_allocator_destroy();
    pmm_init(mb_info);
    paging_init();
    vmm_init();
    heap_init();
    kmalloc_memtest();
}

void init_sched(void) {
    sched_init();
    proc_init();
}

void init_fs(void) {
    vfs_init();
    // procfs init is done within the init process creation
}

typedef enum {
    INIT_STAGE_EARLY,
    INIT_STAGE_CPU,
    INIT_STAGE_BLOCK,
    INIT_STAGE_MEM,
    INIT_STAGE_FS,
    INIT_STAGE_SCHED,
    INIT_STAGE_COUNT
} init_stage_t;

void init(multiboot_info_t* mb_info) {
    for (int stage = 0; stage < INIT_STAGE_COUNT; stage++) {
        switch ((init_stage_t) stage) {
            case INIT_STAGE_EARLY:
                init_early(mb_info);
                break;
            case INIT_STAGE_CPU:
                init_cpu();
                break;
            case INIT_STAGE_BLOCK:
                init_block();
                break;
            case INIT_STAGE_MEM:
                init_mem(mb_info);
                break;
            case INIT_STAGE_FS:
                init_fs();
                break;
            case INIT_STAGE_SCHED:
                init_sched();
                break;
            default:
                break;
        }
    }
}
