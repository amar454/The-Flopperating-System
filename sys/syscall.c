/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

*/

#include "../apps/echo.h"
#include "../mem/alloc.h"
#include "../mem/pmm.h"
#include "../mem/paging.h"
#include "../mem/vmm.h"
#include "../fs/vfs/vfs.h"
#include "../lib/logging.h"
#include "../lib/str.h"
#include "../lib/refcount.h"
#include "../interrupts/interrupts.h"
#include "../task/sync/spinlock.h"
#include "../task/process.h"
#include "../task/sched.h"
#include "../drivers/time/floptime.h"
#include "../drivers/acpi/acpi.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "syscall.h"

// perform the syscall software interrupt
// returns -1 on failure, or the syscall return value
int syscall(syscall_num_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    uint32_t ret;

    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5) : "memory");

    return ret;
}

// fork the current running process
// return pid_t or -1 on failure
pid_t sys_fork(struct syscall_args* args) {
    // no arguments
    if (args->a1 || args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_fork", RED);
        return -1;
    }

    // fetch parent and fork process
    process_t* parent = proc_get_current();
    pid_t child_pid = proc_fork(parent);

    if (child_pid < 0) {
        return -1;
    }

    return child_pid;
}

pid_t sys_dup(struct syscall_args* args) {
    if (!args->a1) {
        log("sys: invalid args passed to sys_dup", RED);
        return -1;
    }

    pid_t pid = (pid_t) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_dup", RED);
        return -1;
    }

    return proc_dup(pid);
}

// open a file; returns fd or -1
int sys_open(struct syscall_args* args) {
    if (!args->a1 || !args->a2) {
        log("sys: invalid args passed to sys_open", RED);
        return -1;
    }

    void* path = (void*) args->a1;
    uint32_t flags = args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_open", RED);
        return -1;
    }

    if (!path) {
        return -1;
    }

    process_t* proc = proc_get_current();

    int fd;

    // iterate through fds to find a free one
    for (fd = 0; fd < MAX_PROC_FDS; fd++) {
        if (proc->fds[fd].node == NULL) {
            break;
        }
    }

    if (fd == MAX_PROC_FDS) {
        return -1; // no space
    }

    // create file object and return the file descriptor
    struct vfs_node* file = vfs_open(path, flags);

    if (!file) {
        return -1;
    }

    proc->fds[fd].node = file;
    return fd;
}

// close an open file descriptor; returns 0 or -1
int sys_close(struct syscall_args* args) {
    if (!args->a1) {
        log("sys: invalid args passed to sys_close", RED);
        return -1;
    }

    int fd = (int) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_close", RED);
        return -1;
    }

    process_t* proc = proc_get_current();

    if (fd < 0 || fd >= MAX_PROC_FDS) {
        return -1;
    }

    struct vfs_file_descriptor* desc = &proc->fds[fd];

    if (!desc) {
        return -1;
    }

    // close the node of the file descriptor
    vfs_close(desc->node);

    // free the descriptor data structure
    kfree(desc, sizeof(struct vfs_file_descriptor));
    proc->fds[fd].node = NULL;
    return 0;
}

// read from fd; returns bytes read or -1
int sys_read(struct syscall_args* args) {
    if (!args->a1 || !args->a2 || !args->a3) {
        log("sys: invalid args passed to sys_read", RED);
        return -1;
    }

    int fd = (int) args->a1;
    void* buf = (void*) args->a2;
    size_t count = (size_t) args->a3;

    if (args->a4 || args->a5) {
        log("sys: invalid args passed to sys_read", RED);
        return -1;
    }

    process_t* proc = proc_get_current();

    if (fd < 0 || fd >= MAX_PROC_FDS) {
        return -1;
    }

    // fetch the file descriptor
    struct vfs_file_descriptor* desc = &proc->fds[fd];

    if (!desc) {
        return -1;
    }

    // read from the node and return read bytes
    return vfs_read(desc->node, buf, count);
}

int sys_copy_file_range(struct syscall_args* args) {
    if (!args->a1 || !args->a2) {
        log("sys: wrong arguments passed to sys_copy_file_range", RED);
        return -1;
    }

    int fd_in = (int) args->a1;
    int fd_out = (int) args->a2;
    size_t count = (size_t) args->a3;

    if (args->a4 || args->a5) {
        log("sys: invalid args passed to sys_copy_file_range", RED);
        return -1;
    }

    process_t* proc = proc_get_current();

    if (fd_in < 0 || fd_in >= MAX_PROC_FDS || fd_out < 0 || fd_out >= MAX_PROC_FDS) {
        return -1;
    }

    // resolve and create src and dst file descriptors
    struct vfs_file_descriptor* src = &proc->fds[fd_in];
    struct vfs_file_descriptor* dst = &proc->fds[fd_out];

    if (!src || !dst || !src->node || !dst->node) {
        return -1;
    }

    // iterate through the file range in chunks
    size_t chunk = 256;

    if (count < chunk) {
        chunk = count;
    }

    unsigned char* buffer = kmalloc(chunk);

    if (!buffer) {
        return -1;
    }

    size_t total = 0;

    // copy chunks until done
    while (total < count) {
        size_t to_read = count - total;

        if (to_read > chunk) {
            to_read = chunk;
        }

        int r = vfs_read(src->node, buffer, to_read);

        if (r <= 0) {
            break;
        }

        int w = vfs_write(dst->node, buffer, r);

        if (w <= 0) {
            break;
        }

        total += w;

        if ((size_t) r < to_read) {
            break;
        }
    }

    // free the buffer and return total bytes copied
    kfree(buffer, sizeof(char) * chunk);
    return total;
}

// seek to offset; returns new offset or -1
int sys_seek(struct syscall_args* args) {
    if (!args->a1 || !args->a2 || !args->a3) {
        log("sys: wrong arguments given to sys_seek", RED);
        return -1;
    }

    int fd = (int) args->a1;
    int offset = (int) args->a2;
    int whence = (int) args->a3;

    if (args->a4 || args->a5) {
        log("sys: invalid args passed to sys_seek", RED);
        return -1;
    }

    process_t* proc = proc_get_current();

    if (fd < 0 || fd >= MAX_PROC_FDS) {
        return -1;
    }

    // fetch the file descriptor
    struct vfs_file_descriptor* desc = &proc->fds[fd];
    if (!desc || !desc->node) {
        return -1;
    }

    // seek to offset of fd node
    return vfs_seek(desc->node, offset, whence);
}

// write to fd; returns bytes written or -1
int sys_write(struct syscall_args* args) {
    if (!args->a1 || !args->a2 || !args->a3) {
        log("sys: wrong args passed to sys_close", RED);
    }

    int fd = (int) args->a1;
    void* buf = (void*) args->a2;
    size_t count = (size_t) args->a3;

    if (args->a4 || args->a5) {
        log("sys: invalid args passed to sys_close", RED);
    }

    process_t* proc = proc_get_current();

    if (fd < 0 || fd >= MAX_PROC_FDS) {
        return -1;
    }

    if (!buf) {
        return -1;
    }

    if (fd == 1) {
        // fd 1 is just stdout
        // use kernel printing backend
        echo(buf, WHITE);
        return 0;
    }

    // fetch the file descriptor
    struct vfs_file_descriptor* desc = &proc->fds[fd];

    if (!desc || !desc->node) {
        return -1;
    }
    return vfs_write(desc->node, buf, count);
}

// print to console
// returns number of bytes printed or -1 on failure
int sys_print(struct syscall_args* args) {
    if (!args->a1) {
        log("sys: wrong args passed to sys_print", RED);
        return -1;
    }

    char* str_ptr = (char*) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_print", RED);
        return -1;
    }

    size_t len = flopstrlen(str_ptr);

    if (len == 0) {
        return 0;
    }

    // printing to stdout is done with sys_write(fd = 1, buf = str_ptr, count = len)
    // TODO: make this cleaner
    struct syscall_args print_args = {.a1 = 1, .a2 = (uint32_t) str_ptr, .a3 = len};
    return sys_write(&print_args);
}

// rollback a failed mmap allocation
static void sys_mmap_internal_rb(vmm_region_t* region, uintptr_t start_va, uintptr_t end_va) {
    for (uintptr_t va = start_va; va < end_va; va += PAGE_SIZE) {
        uintptr_t phys_addr = vmm_resolve(region, va);
        if (phys_addr) {
            vmm_unmap(region, va);
            pmm_free_page((void*) phys_addr);
        }
    }
}

// allocate and map pages for mmap
static int sys_mmap_internal_alloc(
    vmm_region_t* region, uintptr_t base_vaddr, uint32_t length, uint32_t flags, struct vfs_node* node) {
    uintptr_t end_vaddr = base_vaddr + length;
    // iterate through each page and allocate + map
    for (uintptr_t cur_vaddr = base_vaddr; cur_vaddr < end_vaddr; cur_vaddr += PAGE_SIZE) {
        void* phys_page = pmm_alloc_page();

        if (!phys_page) {
            sys_mmap_internal_rb(region, base_vaddr, cur_vaddr);
            return -1;
        }

        // if we have a node, read data into the page
        if (node) {
            int read_bytes = vfs_read(node, phys_page, PAGE_SIZE);
            // zero out the rest of the page if we read less than a page
            if (read_bytes < 0) {
                uint8_t* bp = (uint8_t*) phys_page;

                for (size_t i = 0; i < PAGE_SIZE; i++) {
                    bp[i] = 0;
                };
            }

            else if ((size_t) read_bytes < PAGE_SIZE) {
                uint8_t* bp = (uint8_t*) phys_page;

                for (size_t i = read_bytes; i < PAGE_SIZE; i++) {
                    bp[i] = 0;
                }
            }

        }
        // no node, just zero the page
        else {
            uint8_t* bp = (uint8_t*) phys_page;

            for (size_t i = 0; i < PAGE_SIZE; i++) {
                bp[i] = 0;
            }
        }

        vmm_map(region, cur_vaddr, (uintptr_t) phys_page, flags);
    }

    return 0;
}

// get the virtual address for mmap
static int sys_mmap_internal_get_va(vmm_region_t* region, uint32_t requested_va, uint32_t length, uint32_t* out_va) {
    if (!region || !out_va) {
        return -1;
    }

    // look for a free virtual range
    if (requested_va == 0) {
        uint32_t found_va = vmm_find_free_range(region, length);

        if (!found_va) {
            return -1;
        }

        *out_va = found_va;
        return 0;
    }

    if (requested_va & (PAGE_SIZE - 1)) {
        return -1;
    }

    *out_va = requested_va;
    return 0;
}

// find the vfs_node for mmap and seek to offset
int sys_mmap_internal_find_and_seek(int fd, struct vfs_node** out_node, uint32_t offset) {
    process_t* proc = proc_get_current();

    if (fd < 0 || fd >= MAX_PROC_FDS) {
        return -1;
    }

    // fetch file descriptor
    struct vfs_file_descriptor* descriptor = &proc->fds[fd];

    if (!descriptor || !descriptor->node) {
        return -1;
    }

    *out_node = descriptor->node;

    // seek to offset and return 0
    if (vfs_seek(*out_node, offset, 0) < 0) {
        return -1;
    }

    return 0;
}

// mmap; returns virtual address or -1
int sys_mmap(struct syscall_args* args) {
    if (!args->a1 || !args->a2 || !args->a3 || !args->a4 || !args->a5) {
        log("sys: wrong args passed to sys_mmap", RED);
        return -1;
    }

    uintptr_t addr = (uintptr_t) args->a1;
    uint32_t len = (uint32_t) args->a2;
    uint32_t flags = (uint32_t) args->a3;
    int fd = (int) args->a4;
    uint32_t offset = (uint32_t) args->a5;

    if (len == 0) {
        return -1;
    }

    // mmap length must be page aligned
    len = ALIGN_UP(len, PAGE_SIZE);

    // fetch current process and vm region
    process_t* proc = proc_get_current();
    vmm_region_t* region = proc->region;

    if (!region) {
        return -1;
    }

    // find vfs_node if fd is given
    struct vfs_node* node = NULL;
    if (fd >= 0) {
        if (sys_mmap_internal_find_and_seek(fd, &node, offset) < 0) {
            return -1;
        }
    }

    // get the virtual address to map
    uintptr_t map_start_va;
    if (sys_mmap_internal_get_va(region, addr, len, (uint32_t*) &map_start_va) < 0) {
        return -1;
    }

    // allocate and map pages
    if (sys_mmap_internal_alloc(region, map_start_va, len, flags, node) < 0) {
        return -1;
    }

    // return the starting virtual address
    return map_start_va;
}

// mremap; returns virtual address or -1
int sys_mremap(struct syscall_args* args) {
    if (!args->a1 || !args->a2 || !args->a3 || !args->a4) {
        log("sys: wrong args passed to sys_mremap", RED);
        return -1;
    }

    uintptr_t addr = (uintptr_t) args->a1;
    uint32_t old_len = (uint32_t) args->a2;
    uint32_t new_len = (uint32_t) args->a3;
    uint32_t flags = (uint32_t) args->a4;

    if (args->a5) {
        log("sys: invalid args passed to sys_mremap", RED);
        return -1;
    }

    if (old_len == 0 || new_len == 0) {
        return -1;
    }

    // page align new and old lengths
    old_len = ALIGN_UP(old_len, PAGE_SIZE);
    new_len = ALIGN_UP(new_len, PAGE_SIZE);

    // fetch current process and vm region
    process_t* proc = proc_get_current();
    if (!proc || !proc->region) {
        return -1;
    }

    vmm_region_t* region = proc->region;

    // if same length, nothing to do
    if (new_len == old_len) {
        return addr;
    } else if (new_len < old_len) {
        // shrink
        uintptr_t shrink_start = addr + new_len;
        uintptr_t shrink_end = addr + old_len;
        sys_mmap_internal_rb(region, shrink_start, shrink_end);
        return addr;
    } else {
        // expand
        uintptr_t expand_start = addr + old_len;
        uintptr_t expand_end = addr + new_len;

        // iterate through each page and allocate + map
        for (uintptr_t va = expand_start; va < expand_end; va += PAGE_SIZE) {
            void* phys_page = pmm_alloc_page();
            // allocation failed, rollback and return -1
            if (!phys_page) {
                sys_mmap_internal_rb(region, expand_start, va);
                return -1;
            }
            // zero out the page
            uint8_t* bp = (uint8_t*) phys_page;
            for (size_t i = 0; i < PAGE_SIZE; i++) {
                bp[i] = 0;
            }
            // map the page
            vmm_map(region, va, (uintptr_t) phys_page, flags);
        }
        // return the starting virtual address
        return addr;
    }
}

// validate a memory mapping for munmap
static int sys_munmap_internal_validate(vmm_region_t* region, uintptr_t addr, uint32_t len) {
    if (!region) {
        return -1;
    }

    if (len == 0) {
        return -1;
    }

    if (addr & (PAGE_SIZE - 1)) {
        return -1;
    }

    len = ALIGN_UP(len, PAGE_SIZE);

    return 0;
}

// free physical pages for munmap; also unmaps them
static void sys_munmap_internal_free_phys(vmm_region_t* region, uintptr_t start_va, uintptr_t end_va) {
    for (uintptr_t va = start_va; va < end_va; va += PAGE_SIZE) {
        uintptr_t phys = vmm_resolve(region, va);
        if (phys) {
            vmm_unmap(region, va);
            pmm_free_page((void*) phys);
        }
    }
}

// unmap a memory range for munmap
static int sys_munmap_internal_unmap_range(vmm_region_t* region, uintptr_t addr, uint32_t len) {
    uintptr_t end = addr + len;

    for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
        uintptr_t phys = vmm_resolve(region, va);
        if (!phys) {
            // not mapped
            return -1;
        }
    }

    sys_munmap_internal_free_phys(region, addr, end);
    return 0;
}

// fetch region, validate and unmap range for munmap
static int sys_munmap_internal(process_t* proc, uintptr_t addr, uint32_t len) {
    if (!proc || !proc->region) {
        return -1;
    }

    vmm_region_t* region = proc->region;

    if (sys_munmap_internal_validate(region, addr, len) < 0) {
        return -1;
    }

    len = ALIGN_UP(len, PAGE_SIZE);

    return sys_munmap_internal_unmap_range(region, addr, len);
}

// munmap; returns 0 or -1
int sys_munmap(struct syscall_args* args) {
    if (!args->a1 || !args->a2) {
        log("sys: wrong args passed to sys_munmap", RED);
        return -1;
    }

    uintptr_t addr = (uintptr_t) args->a1;
    uint32_t len = (uint32_t) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_munmap", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }
    return sys_munmap_internal(proc, addr, len);
}

// stat a file; returns 0 or -1
int sys_stat(struct syscall_args* args) {
    if (!args->a1 || !args->a2) {
        log("sys: wrong args passed to sys_stat", RED);
        return -1;
    }

    char* path = (char*) args->a1;
    stat_t* st = (stat_t*) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_stat", RED);
        return -1;
    }
    return vfs_stat(path, st);
}

// fstat a file descriptor; returns 0 or -1
int sys_fstat(struct syscall_args* args) {
    if (!args->a1 || !args->a2) {
        log("sys: wrong args passed to sys_fstat", RED);
        return -1;
    }

    int fd = (int) args->a1;
    stat_t* st = (stat_t*) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_fstat", RED);
        return -1;
    }
    process_t* proc = proc_get_current();

    if (fd < 0 || fd >= MAX_PROC_FDS) {
        return -1;
    }

    struct vfs_file_descriptor* d = &proc->fds[fd];

    if (!d || !d->node) {
        return -1;
    }

    return vfs_fstat(d->node, st);
}

// unlink a file; returns 0 or -1
int sys_unlink(struct syscall_args* args) {
    if (!args->a1) {
        log("sys: wrong args passed to sys_unlink", RED);
        return -1;
    }

    char* path = (char*) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_unlink", RED);
        return -1;
    }

    return vfs_unlink(path);
}

// create a directory; returns 0 or -1
int sys_mkdir(struct syscall_args* args) {
    if (!args->a1 || !args->a2) {
        log("sys: wrong arguments passed to sys_mkdir", RED);
        return -1;
    }

    char* path = (char*) args->a1;
    uint32_t mode = (uint32_t) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_mkdir", RED);
        return -1;
    }

    return vfs_mkdir(path, mode);
}

// remove a directory; returns 0 or -1
int sys_rmdir(struct syscall_args* args) {
    if (!args->a1) {
        log("sys: wrong arguments passed to sys_rmdir", RED);
        return -1;
    }

    char* path = (char*) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_truncate", RED);
        return -1;
    }

    return vfs_rmdir(path);
}

// truncate a file; returns 0 or -1
int sys_truncate(struct syscall_args* args) {
    if (!args->a1 || !args->a2) {
        log("sys: wrong arguments passed to sys_truncate", RED);
        return -1;
    }

    char* path = (char*) args->a1;
    uint64_t length = (uint32_t) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_truncate", RED);
        return -1;
    }

    return vfs_truncate_path(path, length);
}

// ftruncate a file descriptor; returns 0 or -1
int sys_ftruncate(struct syscall_args* args) {
    if (!args->a1 || !args->a2) {
        log("sys: wrong arguments passed to sys_ftruncate", RED);
        return -1;
    }

    int fd = (int) args->a1;
    uint64_t length = (uint64_t) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_ftruncate", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (fd < 0 || fd >= MAX_PROC_FDS) {
        return -1;
    }

    struct vfs_file_descriptor* desc = &proc->fds[fd];
    if (!desc || !desc->node) {
        return -1;
    }

    return vfs_ftruncate(desc->node, length);
}

// rename a file; returns 0 or -1
int sys_rename(struct syscall_args* args) {
    if (!args || !args->a1 || !args->a2) {
        log("sys: invalid args passed to sys_rename", RED);
        return -1;
    }

    char* oldpath = (char*) args->a1;
    char* newpath = (char*) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_rename", RED);
        return -1;
    }

    return vfs_rename(oldpath, newpath);
}

// getpid; returns pid_t or -1 on failure
pid_t sys_getpid(struct syscall_args* args) {
    (void) args;

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }
    return proc->pid;
}

// getuid; returns uid_t or -1 on failure
uid_t sys_getuid(struct syscall_args* args) {
    (void) args;

    process_t* proc = proc_get_current();
    if (!proc) {
        return (uid_t) -1;
    }
    return proc->uid;
}

// get group id; returns gid_t or -1 on failure
pid_t sys_getgid(struct syscall_args* args) {
    (void) args;

    process_t* proc = proc_get_current();
    if (!proc) {
        return (pid_t) -1;
    }
    return proc->gid;
}

// get effective user id; returns uid_t or -1 on failure
uid_t sys_geteuid(struct syscall_args* args) {
    (void) args;

    process_t* proc = proc_get_current();
    if (!proc) {
        return (uid_t) -1;
    }
    return proc->ruid;
}

// get session id; returns pid_t or -1 on failure
pid_t sys_getsid(struct syscall_args* args) {
    (void) args;

    process_t* proc = proc_get_current();
    if (!proc) {
        return (pid_t) -1;
    }
    return proc->sid;
}

// set session id; returns 0 or -1 on failure
int sys_setsid(struct syscall_args* args) {
    if (!args) {
        log("sys: invalid args passed to sys_setsid", RED);
        return -1;
    }

    pid_t sid = (pid_t) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_setsid", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }

    proc->sid = sid;
    return 0;
}

// register user and group ids; returns 0 or -1 on failure
int sys_regidt(struct syscall_args* args) {
    if (!args) {
        log("sys: invalid args passed to sys_regidt", RED);
        return -1;
    }

    pid_t rgid = (pid_t) args->a1;
    pid_t gid = (pid_t) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_regidt", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }

    if (rgid != (pid_t) -1) {
        proc->rgid = rgid;
    }

    if (gid != (pid_t) -1) {
        proc->gid = gid;
    }

    return 0;
}

// set user id of the current process; returns 0 or -1 on failure
int sys_setuid(struct syscall_args* args) {
    if (!args) {
        log("sys: invalid args passed to sys_setuid", RED);
        return -1;
    }

    pid_t ruid = (pid_t) args->a1;
    uid_t uid = (uid_t) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_setuid", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }

    if (ruid != (pid_t) -1) {
        proc->ruid = ruid;
    }

    if (uid != (uid_t) -1) {
        proc->uid = uid;
    }

    return 0;
}

// set group id of the current process; returns 0 or -1 on failure
int sys_setgid(struct syscall_args* args) {
    if (!args) {
        log("sys: invalid args passed to sys_setgid", RED);
        return -1;
    }

    pid_t rgid = (pid_t) args->a1;
    pid_t gid = (pid_t) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_setgid", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }

    if (rgid != (pid_t) -1) {
        proc->rgid = rgid;
    }

    if (gid != (pid_t) -1) {
        proc->gid = gid;
    }

    return 0;
}

// change working directory; returns 0 or -1 on failure
int sys_chdir(struct syscall_args* args) {
    if (!args || !args->a1) {
        log("sys: invalid args passed to sys_chdir", RED);
        return -1;
    }

    char* path = (char*) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_chdir", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }

    struct vfs_node* new_directory = vfs_open(path, 0);
    if (!new_directory) {
        return -1;
    }

    if (proc->cwd) {
        vfs_close(proc->cwd);
    }

    proc->cwd = new_directory;
    return 0;
}

// reboot the system; does not return
// uses qemu hypervisor call
int sys_reboot(struct syscall_args* args) {
    (void) args;
    qemu_power_off();
    return 0;
}

// create a pipe; returns 0 or -1 on failure
int sys_pipe(struct syscall_args* args) {
    if (!args || !args->a1) {
        log("sys: invalid args passed to sys_pipe", RED);
        return -1;
    }

    int* pipefd = (int*) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_pipe", RED);
        return -1;
    }

    pipe_t* p = kmalloc(sizeof(pipe_t));
    if (!p) {
        return -1;
    }
    pipe_init(p);

    process_t* proc = proc_get_current();
    if (!proc) {
        kfree(p, sizeof(pipe_t));
        return -1;
    }

    int read_fd = -1;
    int write_fd = -1;
    for (int i = 0; i < MAX_PROC_FDS; i++) {
        if (proc->fds[i].node == NULL) {
            if (read_fd < 0) {
                read_fd = i;
            } else if (write_fd < 0) {
                write_fd = i;
                break;
            }
        }
    }

    if (read_fd < 0 || write_fd < 0) {
        kfree(p, sizeof(pipe_t));
        return -1;
    }

    proc->fds[read_fd].pipe = p;
    proc->fds[write_fd].pipe = p;

    pipefd[0] = read_fd;
    pipefd[1] = write_fd;

    return 0;
}

// clone a process; returns pid_t or -1 on failure
int sys_clone(struct syscall_args* args) {
    if (!args) {
        log("sys: invalid args passed to sys_clone", RED);
        return -1;
    }

    uint32_t flags = (uint32_t) args->a1;
    void* stack = (void*) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_clone", RED);
        return -1;
    }

    (void) flags;
    (void) stack;

    process_t* parent = proc_get_current();
    if (!parent) {
        return -1;
    }

    pid_t child_pid = proc_fork(parent);
    if (child_pid < 0) {
        return -1;
    }
    return child_pid;
}

// perform an ioctl on a file descriptor; returns result or -1 on failure
int sys_ioctl(struct syscall_args* args) {
    if (!args || !args->a1 || !args->a2) {
        log("sys: invalid args passed to sys_ioctl", RED);
        return -1;
    }

    int fd = (int) args->a1;
    int request = (int) args->a2;
    void* arg = (void*) args->a3;

    if (args->a4 || args->a5) {
        log("sys: invalid args passed to sys_ioctl", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc || fd < 0 || fd >= MAX_PROC_FDS) {
        return -1;
    }

    struct vfs_file_descriptor* desc = &proc->fds[fd];
    if (!desc || !desc->node) {
        return -1;
    }

    if (desc->node->ops->ioctl) {
        return desc->node->ops->ioctl(desc->node, request, (unsigned long) arg);
    }
    return -1;
}

// yield the CPU to another process; returns 0
int sys_sched_yield(struct syscall_args* args) {
    (void) args;
    sched_yield();
    return 0;
}

extern proc_table_t* proc_tbl;

// kill a process; returns 0 or -1 on failure
int sys_kill(struct syscall_args* args) {
    if (!args || !args->a1) {
        log("sys: invalid args passed to sys_kill", RED);
        return -1;
    }

    pid_t pid = (pid_t) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_kill", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }

    process_t* target_proc = proc_get_process_by_pid(pid);
    if (!target_proc) {
        return -1;
    }

    if (target_proc->parent != proc) {
        return -1;
    }

    spinlock(&proc_tbl->proc_table_lock);
    target_proc->state = TERMINATED;
    spinlock_unlock(&proc_tbl->proc_table_lock, true);
    return 0;
}

// open or create a file; returns 0 or -1 on failure
int sys_creat(struct syscall_args* args) {
    if (!args || !args->a1) {
        log("sys: invalid args passed to sys_creat", RED);
        return -1;
    }

    char* path = (char*) args->a1;
    uint32_t mode = (uint32_t) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_creat", RED);
        return -1;
    }

    return vfs_open(path, VFS_MODE_CREATE | VFS_MODE_TRUNCATE | VFS_MODE_RW | mode) ? 0 : -1;
}

// create a hard link; returns 0 or -1 on failure
int sys_link(struct syscall_args* args) {
    if (!args || !args->a1 || !args->a2) {
        log("sys: invalid args passed to sys_link", RED);
        return -1;
    }

    char* oldpath = (char*) args->a1;
    char* newpath = (char*) args->a2;

    if (args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_link", RED);
        return -1;
    }

    return vfs_link(oldpath, newpath);
}

// get maximum priority; returns max priority
int sys_get_priority_max(struct syscall_args* args) {
    (void) args;
    return MAX_PRIORITY;
}

// get minimum priority; returns min priority
int sys_get_priority_min(struct syscall_args* args) {
    (void) args;
    return 0;
}

// mount a filesystem; returns 0 or -1 on failure
int sys_fsmount(struct syscall_args* args) {
    if (!args || !args->a1 || !args->a2) {
        log("sys: invalid args passed to sys_fsmount", RED);
        return -1;
    }

    char* source = (char*) args->a1;
    char* target = (char*) args->a2;
    int flags = (int) args->a3;

    if (args->a4 || args->a5) {
        log("sys: invalid args passed to sys_fsmount", RED);
        return -1;
    }

    return vfs_mount(source, target, flags);
}

// exit all threads in the current process; returns 0 or -1 on failure
int sys_exit_group(struct syscall_args* args) {
    if (!args) {
        log("sys: invalid args passed to sys_exit_group", RED);
        return -1;
    }

    int status = (int) args->a1;

    if (args->a2 || args->a3 || args->a4 || args->a5) {
        log("sys: invalid args passed to sys_exit_group", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc) {
        return -1;
    }

    for (int fd = 0; fd < MAX_PROC_FDS; fd++) {
        if (proc->fds[fd].node) {
            struct syscall_args args = {.a1 = fd};
            sys_close(&args);
        }
    }

    proc_exit_all_threads(proc);
    return 0;
}

// get current working directory; returns vfs_node or NULL on failure
int sys_getcwd(struct syscall_args* args) {
    (void) args;

    process_t* proc = proc_get_current();
    if (!proc || !proc->cwd) {
        return 0;
    }

    return (int) (uintptr_t) proc->cwd;
}

// protect a memory region; returns 0 or -1 on failure
int sys_mprotect(struct syscall_args* args) {
    if (!args || !args->a1 || !args->a2 || !args->a3) {
        log("sys: invalid args passed to sys_mprotect", RED);
        return -1;
    }

    uintptr_t addr = (uintptr_t) args->a1;
    uint32_t len = (uint32_t) args->a2;
    uint32_t flags = (uint32_t) args->a3;

    if (args->a4 || args->a5) {
        log("sys: invalid args passed to sys_mprotect", RED);
        return -1;
    }

    process_t* proc = proc_get_current();
    if (!proc || !proc->region) {
        return -1;
    }

    vmm_region_t* region = proc->region;

    if (len == 0) {
        return -1;
    }

    if (addr & (PAGE_SIZE - 1)) {
        return -1;
    }

    len = ALIGN_UP(len, PAGE_SIZE);
    uintptr_t end = addr + len;

    for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
        uintptr_t phys = vmm_resolve(region, va);

        if (!phys) {
            return -1;
        }
    }

    for (uintptr_t va = addr; va < end; va += PAGE_SIZE) {
        vmm_protect(region, va, flags);
    }

    return 0;
}

// called by the assembly syscall_routine when handling the 0x80 software interrupt
int c_syscall_routine(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    struct syscall_args args = {.a1 = a1, .a2 = a2, .a3 = a3, .a4 = a4, .a5 = a5};

    return syscall_dispatch_table[num](&args);
}
