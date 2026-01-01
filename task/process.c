/*

Copyright 2024, 2025 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

*/

#include "sched.h"
#include "../mem/alloc.h"
#include "../mem/pmm.h"
#include "../mem/paging.h"
#include "../mem/vmm.h"
#include "../mem/utils.h"
#include "../fs/vfs/vfs.h"
#include "../fs/procfs/procfs.h"
#include "../lib/logging.h"
#include "../lib/str.h"
#include "../lib/refcount.h"
#include "../interrupts/interrupts.h"
#include "../task/sync/spinlock.h"
#include "../drivers/time/floptime.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static bool init_process_ran = false;
static process_t* init_process;
proc_table_t* proc_tbl;
static proc_info_t* proc_info_local;
process_t* current_process;

static void proc_info_init() {
    proc_info_local->next_pid = 1;
    proc_info_local->process_count = 0;
    proc_info_local->table = proc_tbl;
}

void proc_table_init() {
    proc_tbl->processes = NULL;
    static spinlock_t proc_lock_init = SPINLOCK_INIT;
    proc_tbl->proc_table_lock = proc_lock_init;
    spinlock_init(&proc_tbl->proc_table_lock);
}

process_t* proc_get_current() {
    return current_process;
}

static void proc_init_process_dummy_entry() {
    if (init_process_ran == false) {
        log("entered init process!", RED);
        init_process_ran = true;
    } else {
        proc_kill(init_process);
    }
}

static void proc_family_init(process_t* process) {
    process->children = NULL;
    process->siblings = NULL;
    process->parent = NULL;
}

// add child to parent's children list
static void proc_family_add_child(process_t* parent, process_t* child) {
    if (!parent || !child) {
        return;
    }

    child->parent = parent;
    child->siblings = parent->children;
    parent->children = child;
}

// Add two children to a parent's children list as twins
static void proc_twin_child_add(process_t* parent, process_t* child1, process_t* child2) {
    if (!parent || !child1 || !child2) {
        return;
    }

    child1->parent = parent;
    child2->parent = parent;
    child1->siblings = child2;
    child2->siblings = parent->children;
    parent->children = child1;
}

// remove child from parent's children list
static void proc_family_remove_child(process_t* parent, process_t* child) {
    if (!parent || !child) {
        return;
    }

    process_t* current = parent->children;
    process_t* prev = NULL;

    while (current) {
        if (current == child) {
            if (prev) {
                prev->siblings = current->siblings;
            } else {
                parent->children = current->siblings;
            }
            child->parent = NULL;
            child->siblings = NULL;
            return;
        }
        prev = current;
        current = current->siblings;
    }
}

static void proc_family_remove_all_children(process_t* parent) {
    if (!parent) {
        return;
    }

    process_t* current = parent->children;
    process_t* next;

    while (current) {
        next = current->siblings;
        current->parent = NULL;
        current->siblings = NULL;
        current = next;
    }

    parent->children = NULL;
}

static void proc_family_transfer_children(process_t* old_parent, process_t* new_parent) {
    if (!old_parent || !new_parent) {
        return;
    }

    process_t* current = old_parent->children;
    process_t* next;

    while (current) {
        next = current->siblings;
        proc_family_add_child(new_parent, current);
        current = next;
    }

    old_parent->children = NULL;
}

static process_t proc_table_static[MAX_PROCESSES];

static process_t* proc_alloc_process_struct() {
    for (int i = 0; i < MAX_PROCESSES; ++i) {
        if (proc_table_static[i].state == 0) {
            flop_memset(&proc_table_static[i], 0, sizeof(process_t));
            return &proc_table_static[i];
        }
    }
    return NULL;
}

static thread_list_t thread_list_pool[MAX_PROCESSES];
static size_t thread_list_index = 0;

static thread_list_t* proc_alloc_thread_list() {
    if (thread_list_index >= MAX_PROCESSES) {
        return NULL;
    }
    return &thread_list_pool[thread_list_index++];
}

static void proc_alloc_assign_ids(process_t* process) {
    process->pid = -1;
    process->sid = -1;
    process->pgid = -1;
    process->rgid = 0;
    process->gid = 0;
    process->ruid = 0;
    process->uid = 0;
}

// this is kinda tricky because the
// caller of this function is supposed to
// set up the rest of the process_t data structure.
// this does nothing besides allocate and zero the data structures behind a process
static process_t* proc_alloc(void) {
    process_t* process = proc_alloc_process_struct();
    if (!process) {
        return NULL;
    }

    // allocate a thread list for the process
    // threads are allocated as they are created
    // meaning we only need a thread list allocation here
    process->threads = proc_alloc_thread_list();
    if (!process->threads) {
        kfree(process, sizeof(process_t));
        return NULL;
    }

    flop_memset(process->threads, 0, sizeof(thread_list_t));

    for (int i = 0; i < MAX_PROC_FDS; ++i) {
        flop_memset(&process->fds[i], 0, sizeof(struct vfs_file_descriptor));
    }

    process->region = NULL;
    process->mem_usage = 0;
    process->parent = NULL;
    process->children = NULL;
    process->siblings = NULL;
    process->name = NULL;

    proc_alloc_assign_ids(process);

    process->state = 0;
    return process;
}

static int proc_cwd_assign(process_t* process, struct vfs_node* cwd) {
    if (!process || !cwd) {
        return -1;
    }

    if (process->cwd) {
        vfs_close(process->cwd);
    }

    process->cwd = cwd;
    refcount_inc_not_zero(&cwd->refcount);
    return 0;
}

static int proc_init_process_zero_ids(process_t* process) {
    if (!process) {
        return -1;
    }

    process->pid = 0;
    process->sid = 0;
    process->pgid = 0;
    process->rgid = 0;
    process->gid = 0;
    process->ruid = 0;
    process->uid = 0;

    return 0;
}

static int proc_init_process_create_region(process_t* process, size_t initial_pages) {
    if (!process) {
        return -1;
    }

    uintptr_t region_va = 0;
    process->region = vmm_region_create(initial_pages, PAGE_USER | PAGE_RW, &region_va);
    if (!process->region) {
        return -1;
    }
    process->mem_usage = initial_pages * PAGE_SIZE;
    return 0;
}

static int proc_init_process_family_create(process_t* init_proc) {
    if (!init_proc || !proc_info_local) {
        return -1;
    }

    init_proc->parent = NULL;
    init_proc->children = NULL;
    init_proc->siblings = NULL;

    init_proc->pid = proc_info_local->next_pid++;
    proc_info_local->process_count++;

    return 0;
}

static void proc_init_process_free_data_structures(process_t* process) {
    if (!process) {
        return;
    }

    if (process->cwd) {
        vfs_close(process->cwd);
    }

    if (process->region) {
        vmm_region_destroy(process->region);
    }

    if (process->name) {
        kfree(process->name, flopstrlen(process->name) + 1);
    }

    if (process->threads) {
        kfree(process->threads, sizeof(thread_list_t));
    }

    kfree(process, sizeof(process_t));
    return;
}

int proc_init_process_create_thread(process_t* process,
                                    void (*entry)(void),
                                    unsigned priority,
                                    const char* thread_name) {
    if (!process || !entry || !thread_name) {
        return -1;
    }

    thread_t* thread = sched_create_user_thread(entry, priority, (char*) thread_name, process);
    if (!thread) {
        return -1;
    }

    sched_thread_list_add(thread, process->threads);
    return 0;
}

static char init_process_name_static[] = "init_process";

int proc_init_process_assign_name(process_t* process, const char* name) {
    process->name = init_process_name_static;
    return 0;
}

void proc_signal_init_process_init(process_t* process) {
    if (!process) {
        return;
    }

    spinlock_init(&process->sig_lock);

    spinlock(&process->sig_lock);

    process->sig_pending = 0;
    process->sig_mask = 0;

    for (int i = 0; i < SIGMAX; i++) {
        process->sig_handlers[i] = NULL;
    }

    spinlock_unlock(&process->sig_lock, true);
}

int proc_create_init_process() {
    spinlock(&proc_tbl->proc_table_lock);

    init_process = proc_alloc();
    if (!init_process) {
        log("init_process alloc failed\n", RED);
        spinlock_unlock(&proc_tbl->proc_table_lock, true);
        return -1;
    }

    init_process->state = EMBRYO;
    spinlock_unlock(&proc_tbl->proc_table_lock, true);

    proc_signal_init_process_init(init_process);

    if (proc_init_process_zero_ids(init_process) < 0) {
        log("init_process zero_ids failed\n", RED);
        proc_init_process_free_data_structures(init_process);
        return -1;
    }

    if (proc_init_process_assign_name(init_process, "init_process") < 0) {
        log("init_process assign_name failed\n", RED);
        proc_init_process_free_data_structures(init_process);
        return -1;
    }

    if (proc_init_process_create_region(init_process, 4) < 0) {
        log("init_process create_region failed\n", RED);
        proc_init_process_free_data_structures(init_process);
        return -1;
    }

    init_process->mem_usage = 4 * PAGE_SIZE;

    if (proc_init_process_family_create(init_process) < 0) {
        log("init_process family_create failed\n", RED);
        proc_init_process_free_data_structures(init_process);
        return -1;
    }

    init_process->cwd = NULL;

    procfs_init();
    procfs_add_entry("init_process", VFS_TYPE_PROCFS);

    if (proc_init_process_create_thread(init_process, proc_init_process_dummy_entry, 0, "init_thread") < 0) {
        log("init_process create_thread failed\n", RED);
        proc_init_process_free_data_structures(init_process);
        return -1;
    }

    spinlock(&proc_tbl->proc_table_lock);
    init_process->state = RUNNING;
    spinlock_unlock(&proc_tbl->proc_table_lock, true);

    log("init_process successfully created\n", GREEN);
    return 0;
}

pid_t proc_getpid(process_t* process) {
    if (!process) {
        return -1;
    }
    return process->pid;
}

int proc_kill(process_t* process) {
    if (!process) {
        return -1;
    }

    return signal_send(process, SIGKILL);
}

int proc_exit_all_threads(process_t* process) {
    if (!process) {
        return -1;
    }

    return signal_send(process, SIGTERM);
}

int proc_exit(process_t* process, int status) {
    if (!process) {
        return -1;
    }

    process->exit_status = status;
    return signal_send(process, SIGTERM);
}

int proc_stop(process_t* process) {
    if (!process) {
        return -1;
    }

    return signal_send(process, SIGSTOP);
}

int proc_continue(process_t* process) {
    if (!process) {
        return -1;
    }

    return signal_send(process, SIGCONT);
}

static int proc_copy_fds(process_t* dest, process_t* src) {
    if (!dest || !src) {
        return -1;
    }

    for (int i = 0; i < MAX_PROC_FDS; ++i) {
        flop_memcpy(&dest->fds[i], &src->fds[i], sizeof(struct vfs_file_descriptor));

        void* node_ptr = dest->fds[i].node;
        if (!node_ptr) {
            continue;
        }

        if (((struct vfs_node*) node_ptr)->pipe.data == (void*) &((struct vfs_node*) node_ptr)->pipe) {
            pipe_t* p = (pipe_t*) node_ptr;
            int flags = dest->fds[i].node->vfs_mode;

            if ((flags & VFS_MODE_R) || (flags & VFS_MODE_RW)) {
                if (!pipe_dup_read(p)) { // increase read refcount
                    return -1;
                }
            }
            if ((flags & VFS_MODE_W) || (flags & VFS_MODE_RW)) {
                if (!pipe_dup_write(p)) { // increase write refcount
                    return -1;
                }
            }
        } else {
            refcount_inc_not_zero(&((struct vfs_node*) node_ptr)->refcount);
        }
    }

    return 0;
}

#define SIG_ITERATE                                                                                                    \
    for (int i = 0; i < SIGMAX; i++) {                                                                                 \
        child->sig_handlers[i] = parent->sig_handlers[i];                                                              \
    }

static void proc_copy_signal_state(process_t* parent, process_t* child) {
    spinlock(&parent->sig_lock);
    spinlock(&child->sig_lock);

    child->sig_mask = parent->sig_mask;
    child->sig_pending = 0;

    SIG_ITERATE;

    spinlock_unlock(&child->sig_lock, true);
    spinlock_unlock(&parent->sig_lock, true);
}

static int proc_copy_child_pagemap(process_t* parent, process_t* child) {
    if (!parent || !child) {
        return -1;
    }

    child->region = vmm_copy_pagemap(parent->region);
    if (!child->region) {
        return -1;
    }
    child->mem_usage = parent->mem_usage;
    return 0;
}

static int proc_assign_child_ids(process_t* parent, process_t* child) {
    if (!parent || !child || !proc_info_local) {
        return -1;
    }

    child->pid = proc_info_local->next_pid++;
    child->sid = parent->sid;
    child->pgid = parent->pgid;
    child->rgid = parent->rgid;
    child->gid = parent->gid;
    child->ruid = parent->ruid;
    child->uid = parent->uid;

    proc_info_local->process_count++;

    child->siblings = parent->children;
    parent->children = child;

    return 0;
}

static void proc_fork_failed_child_data_structures(process_t* child) {
    if (!child) {
        return;
    }

    if (child->cwd) {
        vfs_close(child->cwd);
    }

    if (child->region) {
        vmm_region_destroy(child->region);
    }

    if (child->name) {
        kfree(child->name, flopstrlen(child->name) + 1);
    }

    if (child->threads) {
        kfree(child->threads, sizeof(thread_list_t));
    }

    kfree(child, sizeof(process_t));
    return;
}

// to be called from sys_fork()
// creates a child process that is a copy of the parent process
// returns the child's pid on success, -1 on failure
pid_t proc_fork(process_t* parent) {
    if (!parent || !proc_info_local || !proc_tbl) {
        return -1;
    }

    process_t* child;

    if (!(child = proc_alloc())) {
        return -1;
    }

    if (proc_copy_child_pagemap(parent, child) < 0) {
        proc_fork_failed_child_data_structures(child);
        return -1;
    }

    if (proc_cwd_assign(child, parent->cwd) < 0) {
        proc_fork_failed_child_data_structures(child);
        return -1;
    }

    if (proc_copy_fds(child, parent) < 0) {
        proc_fork_failed_child_data_structures(child);
        return -1;
    }

    // TODO: figure out a better way to do the name.
    // allocating for a temporary name seems avoidable
    char* child_name = parent->name ? parent->name : "__embryo_process";
    size_t name_len = flopstrlen(child_name) + 1;
    child->name = (char*) kmalloc(name_len);

    if (!child->name) {
        proc_fork_failed_child_data_structures(child);
        return -1;
    }

    flopstrcopy(child->name, child_name, name_len);

    spinlock(&proc_tbl->proc_table_lock);

    if (proc_assign_child_ids(parent, child) < 0) {
        spinlock_unlock(&proc_tbl->proc_table_lock, true);
        proc_fork_failed_child_data_structures(child);
        return -1;
    }
    // back pointer
    child->parent = parent;
    child->state = RUNNABLE;

    spinlock_unlock(&proc_tbl->proc_table_lock, true);

    proc_copy_signal_state(parent, child);

    return child->pid;
}

pid_t proc_dup(pid_t pid) {
    process_t* parent = NULL;
    process_t* child = NULL;

    spinlock(&proc_tbl->proc_table_lock);

    process_t* iter_process = proc_tbl->processes;
    while (iter_process) {
        if (iter_process->pid == pid) {
            parent = iter_process;
            break;
        }
        iter_process = iter_process->siblings;
    }

    spinlock_unlock(&proc_tbl->proc_table_lock, true);

    if (!parent) {
        return -1;
    }

    pid_t child_pid = proc_fork(parent);
    if (child_pid < 0) {
        return -1;
    }

    return child_pid;
}

int proc_init() {
    static proc_info_t proc_info_instance;
    static proc_table_t proc_table_instance;

    proc_info_local = &proc_info_instance;
    proc_tbl = &proc_table_instance;

    proc_info_init();
    proc_table_init();

    if (proc_create_init_process() < 0) {
        log("proc_init: failed to create init process\n", RED);
        return -1;
    }

    log("proc: init - ok\n", GREEN);
    return 0;
}

void* proc_alloc_within_process(process_t* process, size_t size) {
    if (!process || size == 0) {
        return NULL;
    }

    size_t pages = size / PAGE_SIZE;
    if (size % PAGE_SIZE != 0) {
        pages += 1;
    }

    uintptr_t alloc_addr = vmm_alloc(process->region, pages, PAGE_USER | PAGE_RW);
    if (alloc_addr == 0) {
        return NULL;
    }

    process->mem_usage += pages * PAGE_SIZE;
    return (void*) alloc_addr;
}

void proc_free_within_process(process_t* process, uintptr_t addr, size_t size) {
    if (!process || addr == 0 || size == 0) {
        return;
    }

    size_t pages = size / PAGE_SIZE;
    if (size % PAGE_SIZE != 0) {
        pages += 1;
    }

    vmm_unmap(process->region, addr);
    process->mem_usage -= pages * PAGE_SIZE;
}

void* proc_zero_process_memory(process_t* process, size_t size) {
    if (!process || size == 0) {
        return NULL;
    }

    void* addr = proc_alloc_within_process(process, size);
    if (!addr) {
        return NULL;
    }

    flop_memset(addr, 0, size);
    return addr;
}

process_t* proc_get_init_process() {
    return init_process;
}

proc_info_t* proc_get_proc_info() {
    return proc_info_local;
}

proc_table_t* proc_get_proc_table() {
    return proc_tbl;
}

process_t* proc_get_process_by_pid(pid_t pid) {
    if (!proc_tbl) {
        return NULL;
    }

    spinlock(&proc_tbl->proc_table_lock);

    process_t* iter_process = proc_tbl->processes;
    while (iter_process) {
        if (iter_process->pid == pid) {
            spinlock_unlock(&proc_tbl->proc_table_lock, true);
            return iter_process;
        }
        iter_process = iter_process->siblings;
    }

    spinlock_unlock(&proc_tbl->proc_table_lock, true);
    return NULL;
}

static void proc_reparent_children(process_t* old_parent, process_t* new_parent) {
    if (!old_parent || !new_parent) {
        return;
    }

    process_t* iter_child = old_parent->children;
    process_t* next_child;

    while (iter_child) {
        next_child = iter_child->siblings;
        proc_family_add_child(new_parent, iter_child);
        iter_child = next_child;
    }

    old_parent->children = NULL;
}

static int proc_terminate_all_threads(process_t* process) {
    if (!process || !process->threads) {
        return -1;
    }

    thread_t* iter_thread = process->threads->head;
    while (iter_thread) {
        thread_t* next_thread = iter_thread->next;
        sched_remove(&sched.ready_queue, iter_thread);
        sched_remove(&sched.sleep_queue, iter_thread);
        iter_thread = next_thread;
    }

    process->threads->head = NULL;
    process->threads->tail = NULL;
    return 0;
}

static int proc_clean(process_t* process) {
    if (!process) {
        return -1;
    }

    if (process->cwd) {
        vfs_close(process->cwd);
        process->cwd = NULL;
    }

    if (process->region) {
        vmm_region_destroy(process->region);
        process->region = NULL;
    }

    if (process->name) {
        kfree(process->name, flopstrlen(process->name) + 1);
        process->name = NULL;
    }

    if (process->threads) {
        kfree(process->threads, sizeof(thread_list_t));
        process->threads = NULL;
    }

    return 0;
}

static int proc_remove_process_from_table(process_t* process) {
    if (!proc_tbl || !proc_tbl->processes || !process) {
        return -1;
    }

    spinlock(&proc_tbl->proc_table_lock);

    process_t* iter_process = proc_tbl->processes;
    process_t* prev_process = NULL;

    while (iter_process) {
        if (iter_process == process) {
            if (prev_process) {
                prev_process->siblings = iter_process->siblings;
            } else {
                proc_tbl->processes = iter_process->siblings;
            }

            spinlock_unlock(&proc_tbl->proc_table_lock, true);
            return 0;
        }

        prev_process = iter_process;
        iter_process = iter_process->siblings;
    }

    spinlock_unlock(&proc_tbl->proc_table_lock, true);
    return -1;
}

static int proc_terminate_process(process_t* process) {
    if (!process) {
        return -1;
    }

    proc_terminate_all_threads(process);
    proc_clean(process);
    proc_remove_process_from_table(process);

    spinlock(&proc_tbl->proc_table_lock);
    if (proc_info_local && proc_info_local->process_count > 0) {
        proc_info_local->process_count--;
    }
    spinlock_unlock(&proc_tbl->proc_table_lock, true);

    kfree(process, sizeof(process_t));
    return 0;
}

static int proc_kill_all_children(process_t* parent) {
    if (!parent) {
        return -1;
    }

    process_t* iter_child = parent->children;
    process_t* next_child;

    while (iter_child) {
        next_child = iter_child->siblings;
        proc_terminate_process(iter_child);
        iter_child = next_child;
    }

    parent->children = NULL;
    return 0;
}

static int proc_copy_process_memory(process_t* source_process, process_t* target_process) {
    if (!source_process || !target_process) {
        return -1;
    }

    if (!source_process->region) {
        return -1;
    }

    target_process->region = vmm_copy_pagemap(source_process->region);
    if (!target_process->region) {
        return -1;
    }

    target_process->mem_usage = source_process->mem_usage;
    return 0;
}

static int proc_duplicate_process_fds(process_t* source_process, process_t* target_process) {
    if (!source_process || !target_process) {
        return -1;
    }

    for (int i = 0; i < MAX_PROC_FDS; ++i) {
        flop_memcpy(&target_process->fds[i], &source_process->fds[i], sizeof(struct vfs_file_descriptor));
        if (target_process->fds[i].node) {
            refcount_inc_not_zero(&target_process->fds[i].node->refcount);
        }
    }
    return 0;
}

static int proc_set_parent_process(process_t* process, process_t* parent) {
    if (!process) {
        return -1;
    }

    process->parent = parent;
    if (parent) {
        proc_family_add_child(parent, process);
    }

    return 0;
}

void proc_debug_dump_process_info(process_t* process) {
    if (!process) {
        log("proc_debug_dump_process_info: process is NULL\n", RED);
        return;
    }

    char buffer[256];
    int len = flopsnprintf(buffer,
                           sizeof(buffer),
                           "process info:\n"
                           "pid: %d\n"
                           "name: %s\n"
                           "state: %d\n"
                           "memory usage: %u bytes\n"
                           "cwd: %p\n",
                           process->pid,
                           process->name ? process->name : "NULL",
                           process->state,
                           process->mem_usage,
                           (void*) process->cwd);
    log(buffer, GREEN);
}

void proc_debug_dump_all_processes() {
    spinlock(&proc_tbl->proc_table_lock);

    process_t* current = proc_tbl->processes;
    while (current) {
        proc_debug_dump_process_info(current);
        current = current->siblings;
    }

    spinlock_unlock(&proc_tbl->proc_table_lock, true);
}

void proc_fetch_mem_usage_all_processes() {
    uint32_t total_mem_usage = 0;

    spinlock(&proc_tbl->proc_table_lock);

    process_t* current = proc_tbl->processes;
    while (current) {
        total_mem_usage += current->mem_usage;
        current = current->siblings;
    }

    spinlock_unlock(&proc_tbl->proc_table_lock, true);

    char buffer[128];
    flopsnprintf(buffer, sizeof(buffer), "Total memory usage by all processes: %u bytes\n", total_mem_usage);
    log(buffer, YELLOW);
}

void proc_print_process_thread_id(process_t* process) {
    char buffer[256];
    flopsnprintf(buffer, sizeof(buffer), "process pid: %d, threads:\n", process->pid);
    log(buffer, CYAN);

    spinlock(&process->threads->lock);
    for (thread_t* t = process->threads->head; t; t = t->next) {
        char tbuffer[128];
        flopsnprintf(tbuffer, sizeof(tbuffer), " - thread id: %d, name: %s\n", t->id, t->name);
        log(tbuffer, CYAN);
    }
    spinlock_unlock(&process->threads->lock, true);
}

void proc_print_all_process_thread_ids() {
    spinlock(&proc_tbl->proc_table_lock);

    process_t* current = proc_tbl->processes;
    while (current) {
        proc_print_process_thread_id(current);
        current = current->siblings;
    }

    spinlock_unlock(&proc_tbl->proc_table_lock, true);
}

void proc_print_process_manager_stats() {
    char buffer[128];
    flopsnprintf(buffer,
                 sizeof(buffer),
                 "Process Manager Stats:\nTotal Processes: %d\n",
                 proc_info_local ? proc_info_local->process_count : 0);
    log(buffer, YELLOW);

    proc_debug_dump_all_processes();

    proc_fetch_mem_usage_all_processes();

    proc_print_all_process_thread_ids();
}
