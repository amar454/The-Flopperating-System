/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
#include "../task/sched.h"
#include "../drivers/io/io.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/ata/ata.h"
#include "../drivers/acpi/acpi.h"
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
#include "../sys/syscall.h"
#include "../fs/tmpflopfs/tmpflopfs.h"
#include "../mem/utils.h"
#include "../kernel/kernel.h"
#include "../multiboot/multiboot.h"
extern void framebuffer_term_init();

void init_stage_early(multiboot_info_t* mb_info) {
    framebuffer_init(mb_info);
    framebuffer_term_init();
    log("init: initializing early stage\n", LIGHT_GRAY);
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
    // to be destroyed in init_stage_mem()
    log("init: early stage init - ok\n", LIGHT_GRAY);
}

void init_stage_cpu(void) {
    log("init: initializing cpu stage\n", LIGHT_GRAY);
    gdt_init();
    sleep_seconds(1);
    interrupts_init();
    log("init: cpu stage init - ok\n", LIGHT_GRAY);
}

void init_stage_block() {
    log("init: initializing block stage\n", LIGHT_GRAY);
    ata_init();
    sleep_seconds(1);
    log("init: block stage init - ok\n", LIGHT_GRAY);
}

void init_stage_mem(multiboot_info_t* mb_info) {
    log("init: initializing mem stage\n", LIGHT_GRAY);
    early_allocator_destroy();
    pmm_init(mb_info);
    paging_init();
    vmm_init();
    heap_init();
    kmalloc_memtest();
    log("init: mem stage init - ok\n", LIGHT_GRAY);
}

void init_stage_middle() {
    log("init: initializing middle stage\n", LIGHT_GRAY);
    keyboard_init();
    log("init: middle stage init - ok\n", LIGHT_GRAY);
}

void init_stage_task(void) {
    log("init: initializing task stage\n", LIGHT_GRAY);
    sched_init();
    proc_init();
    log("init: task stage init - ok\n", LIGHT_GRAY);
}

void init_stage_fs(void) {
    log("init: initializing fs stage\n", LIGHT_GRAY);
    vfs_init();
    // procfs init is done within the init process creation
    log("init: fs stage init - ok\n", LIGHT_GRAY);
}

void init_stage_sys() {
    log("init: initializing sys stage\n", LIGHT_GRAY);
    syscall_init();
    log("init: sys stage init - ok\n", LIGHT_GRAY);
}

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

void init(multiboot_info_t* mb_info) {
    for (int stage = 0; stage < INIT_STAGE_COUNT; stage++) {
        switch ((init_stage_t) stage) {
            case INIT_STAGE_EARLY:
                init_stage_early(mb_info);
                break;
            case INIT_STAGE_CPU:
                init_stage_cpu();
                break;
            case INIT_STAGE_BLOCK:
                init_stage_block();
                break;
            case INIT_STAGE_MIDDLE:
                init_stage_middle();
                break;
            case INIT_STAGE_MEM:
                init_stage_mem(mb_info);
                break;
            case INIT_STAGE_FS:
                init_stage_fs();
                break;
            case INIT_STAGE_TASK:
                init_stage_task();
                break;
            case INIT_STAGE_SYS:
                init_stage_sys();
                break;
            default:
                break;
        }
    }
}
