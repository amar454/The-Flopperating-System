/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
#include "../vfs/vfs.h"
#include "../../lib/logging.h"
#include "../../lib/refcount.h"
#include "../../mem/alloc.h"
#include "../../mem/paging.h"
#include "../../mem/utils.h"
#include "../../mem/vmm.h"
#include "../../drivers/vga/vgahandler.h"
#include "../../lib/str.h"
#include "../../task/sync/spinlock.h"
#include <stddef.h>
#include <stdint.h>

struct procfs {
    uint32_t procfs_count;
    struct vfs_op_tbl procfs_ops;
    struct vfs_fs* procfs_fs;
    spinlock_t procfs_lock;
    struct vfs_directory_entry* procfs_dir_entries;
};

static struct procfs pfs;

static struct vfs_directory_list* procfs_build_dirlist() {
    struct vfs_directory_list* list = kmalloc(sizeof(struct vfs_directory_list));
    if (!list) {
        log("procfs: failed to allocate memory for directory list\n", RED);
        return NULL;
    }

    list->head = pfs.procfs_dir_entries;
    list->tail = NULL;

    struct vfs_directory_entry* iter = list->head;
    while (iter) {
        list->tail = iter;
        iter = iter->next;
    }

    return list;
}

void procfs_add_entry(const char* name, int type) {
    struct vfs_directory_entry* entry = kmalloc(sizeof(struct vfs_directory_entry));
    if (!entry) {
        log("procfs: failed to allocate memory for directory entry\n", RED);
        return;
    }

    flopstrcopy(entry->name, name, VFS_MAX_FILE_NAME);
    entry->type = type;
    entry->next = NULL;

    spinlock(&pfs.procfs_lock);
    if (!pfs.procfs_dir_entries) {
        pfs.procfs_dir_entries = entry;
    } else {
        struct vfs_directory_entry* iter = pfs.procfs_dir_entries;
        while (iter->next) {
            iter = iter->next;
        }
        iter->next = entry;
    }
    pfs.procfs_count++;
    spinlock_unlock(&pfs.procfs_lock, true);
}

static struct vfs_node* procfs_open(struct vfs_node* node, char* path) {
    return node;
}

static int procfs_close(struct vfs_node* node) {
    return 0;
}

static int procfs_read(struct vfs_node* node, unsigned char* buf, unsigned long size) {
    if (!node || !buf || size == 0) {
        return 0;
    }

    size_t len = flopstrlen(node->name);
    if (len > size) {
        len = size;
    }
    flopstrcopy((char*) buf, node->name, len);
    return (int) len;
}

static int procfs_write(struct vfs_node* node, unsigned char* buf, unsigned long size) {
    return -1;
}

static void* procfs_mount(char* dev, char* path, int flags) {
    log("procfs: mount called\n", 0x0F);

    if (!pfs.procfs_fs) {
        pfs.procfs_fs = kmalloc(sizeof(struct vfs_fs));
        if (!pfs.procfs_fs) {
            log("procfs: failed to allocate memory for filesystem\n", 0x0F);
            return NULL;
        }

        pfs.procfs_fs->filesystem_type = VFS_FS_PROCFS;
        pfs.procfs_fs->op_table = pfs.procfs_ops;
        pfs.procfs_fs->previous = NULL;
    }

    return pfs.procfs_fs;
}

// a bunch of garbage
static int procfs_unmount(struct vfs_mountpoint* mp, char* path) {
    return 0;
}

static int procfs_create(struct vfs_mountpoint* mp, char* name) {
    return -1;
}

static int procfs_delete(struct vfs_mountpoint* mp, char* name) {
    return -1;
}

static int procfs_unlink(struct vfs_mountpoint* mp, char* name) {
    return -1;
}

static int procfs_mkdir(struct vfs_mountpoint* mp, char* name, uint32_t flags) {
    return -1;
}

static int procfs_rmdir(struct vfs_mountpoint* mp, char* name) {
    return -1;
}

static int procfs_rename(struct vfs_mountpoint* mp, char* oldname, char* newname) {
    return -1;
}

static int procfs_ctrl(struct vfs_node* node, unsigned long cmd, unsigned long arg) {
    return -1;
}

static int procfs_seek(struct vfs_node* node, unsigned long offset, unsigned char whence) {
    return -1;
}

static struct vfs_directory_list* procfs_listdir(struct vfs_mountpoint* mp, char* path) {
    return procfs_build_dirlist();
}

static int procfs_stat(const char* path, struct stat* st) {
    if (!st) {
        return -1;
    }
    flop_memset(st, 0, sizeof(struct stat));
    st->st_mode = 0x4000;
    st->st_size = 0;
    return 0;
}

static int procfs_fstat(struct vfs_node* node, struct stat* st) {
    return procfs_stat(node ? node->name : NULL, st);
}

static int procfs_lstat(const char* path, struct stat* st) {
    return procfs_stat(path, st);
}

static int procfs_truncate(struct vfs_node* node, uint64_t length) {
    return -1;
}

static int procfs_ioctl(struct vfs_node* node, unsigned long cmd, unsigned long arg) {
    return -1;
}

static int procfs_link(struct vfs_mountpoint* mp, char* oldname, char* newname) {
    return -1;
}

void procfs_init() {
    spinlock_init(&pfs.procfs_lock);
    pfs.procfs_count = 0;
    pfs.procfs_dir_entries = NULL;

    procfs_add_entry("cpuinfo", VFS_FILE);
    procfs_add_entry("meminfo", VFS_FILE);

    pfs.procfs_ops.open = procfs_open;
    pfs.procfs_ops.close = procfs_close;
    pfs.procfs_ops.read = procfs_read;
    pfs.procfs_ops.write = procfs_write;
    pfs.procfs_ops.mount = procfs_mount;
    pfs.procfs_ops.unmount = procfs_unmount;
    pfs.procfs_ops.create = procfs_create;
    pfs.procfs_ops.delete = procfs_delete;
    pfs.procfs_ops.unlink = procfs_unlink;
    pfs.procfs_ops.mkdir = procfs_mkdir;
    pfs.procfs_ops.rmdir = procfs_rmdir;
    pfs.procfs_ops.rename = procfs_rename;
    pfs.procfs_ops.ctrl = procfs_ctrl;
    pfs.procfs_ops.seek = procfs_seek;
    pfs.procfs_ops.listdir = procfs_listdir;
    pfs.procfs_ops.stat = procfs_stat;
    pfs.procfs_ops.fstat = procfs_fstat;
    pfs.procfs_ops.lstat = procfs_lstat;
    pfs.procfs_ops.truncate = procfs_truncate;
    pfs.procfs_ops.ioctl = procfs_ioctl;
    pfs.procfs_ops.link = procfs_link;

    vfs_mount("/", "/proc/", VFS_FS_PROCFS);
    log("procfs: init - ok\n", GREEN);
}
