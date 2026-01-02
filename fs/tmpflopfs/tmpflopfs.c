/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later veregion_startion.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

*/
#include "../vfs/vfs.h"
#include "tmpflopfs.h"
#include "../../lib/logging.h"
#include "../../lib/refcount.h"
#include "../../mem/alloc.h"
#include "../../mem/utils.h"
#include "../../mem/paging.h"
#include "../../mem/vmm.h"
#include "../../mem/pmm.h"
#include "../../drivers/vga/vgahandler.h"
#include "../../lib/str.h"
#include "../../task/sync/spinlock.h"
#include <stdint.h>
#include <stddef.h>

static struct vfs_fs tmpfs_fs;

static tmpfs_inode_t* tmpfs_inode_new(const char* name, int type) {
    tmpfs_inode_t* n = (tmpfs_inode_t*) kmalloc(sizeof(tmpfs_inode_t));
    if (!n) {
        return NULL;
    }
    flop_memset(n, 0, sizeof(*n));
    if (name) {
        size_t ln = flopstrlen(name);
        if (ln >= VFS_MAX_FILE_NAME) {
            ln = VFS_MAX_FILE_NAME - 1;
        }
        flopstrcopy(n->name, name, ln + 1);
    }
    n->type = type;
    n->pages = NULL;
    n->page_count = 0;
    n->size = 0;
    return n;
}

static void tmpfs_free_node_pages(tmpfs_inode_t* f) {
    if (!f || !f->pages) {
        return;
    }
    for (size_t i = 0; i < f->page_count; ++i) {
        if (f->pages[i]) {
            pmm_free_page(f->pages[i]);
        }
    }
    kfree(f->pages, f->page_count * sizeof(void*));
    f->pages = NULL;
    f->page_count = 0;
    f->size = 0;
}

static int tmpfs_resize_pages(tmpfs_inode_t* f, size_t new_pages) {
    size_t old_pages = f->page_count;
    if (new_pages == old_pages) {
        return 0;
    }

    void** np = NULL;
    if (new_pages) {
        np = (void**) kmalloc(new_pages * sizeof(void*));
        if (!np) {
            return -1;
        }
        flop_memset(np, 0, new_pages * sizeof(void*));
    }

    size_t keep = (old_pages < new_pages) ? old_pages : new_pages;
    for (size_t i = 0; i < keep; ++i) {
        np[i] = f->pages ? f->pages[i] : NULL;
    }

    for (size_t i = new_pages; i < old_pages; ++i) {
        if (f->pages && f->pages[i]) {
            pmm_free_page(f->pages[i]);
        }
    }

    for (size_t i = old_pages; i < new_pages; ++i) {
        np[i] = pmm_alloc_page();
        if (!np[i]) {
            for (size_t j = old_pages; j < i; ++j) {
                if (np[j]) {
                    pmm_free_page(np[j]);
                }
            }
            if (np) {
                kfree(np, new_pages * sizeof(void*));
            }
            return -1;
        }
        flop_memset(np[i], 0, PAGE_SIZE);
    }

    if (f->pages) {
        kfree(f->pages, old_pages * sizeof(void*));
    }

    f->pages = np;
    f->page_count = new_pages;
    return 0;
}

static inline size_t tmpfs_ceil_div(size_t x, size_t y) {
    return (x + y - 1) / y;
}

static inline int tmpfs_is_sep(char c) {
    return c == '/' || c == '\\';
}

static tmpfs_dirent_t* tmpfs_dirent_prepend(tmpfs_inode_t* dir, tmpfs_inode_t* child) {
    tmpfs_dirent_t* d = (tmpfs_dirent_t*) kmalloc(sizeof(tmpfs_dirent_t));
    if (!d) {
        return NULL;
    }
    d->child = child;
    d->next = dir->children;
    dir->children = d;
    return d;
}

static void tmpfs_dirent_remove(tmpfs_inode_t* dir, tmpfs_inode_t* child) {
    tmpfs_dirent_t *p = NULL, *c = dir->children;
    while (c) {
        if (c->child == child) {
            if (p) {
                p->next = c->next;
            } else {
                dir->children = c->next;
            }
            kfree(c, sizeof(*c));
            break;
        }
        p = c;
        c = c->next;
    }
}

static tmpfs_inode_t* tmpfs_find_child(tmpfs_inode_t* dir, const char* name) {
    tmpfs_dirent_t* d = dir->children;
    while (d) {
        if (flopstrcmp(d->child->name, name) == 0) {
            return d->child;
        }
        d = d->next;
    }
    return NULL;
}

/* Return parent dir and leaf name for rel path */
static tmpfs_inode_t* tmpfs_parent_and_leaf(tmpfs_inode_t* root, const char* path, char* leaf_out) {
    const char* p = path;
    size_t n = 0;
    char leaf[VFS_MAX_FILE_NAME];
    tmpfs_inode_t* cur = root;
    const char* last = NULL;
    const char* it = p;
    while (*it) {
        while (*it && tmpfs_is_sep(*it)) {
            it++;
        }
        if (!*it) {
            break;
        }
        last = it;
        while (*it && !tmpfs_is_sep(*it)) {
            it++;
        }
    }
    if (!last) {
        leaf_out[0] = '\0';
        return root;
    }
    n = 0;
    while (last[n] && !tmpfs_is_sep(last[n]) && n < sizeof(leaf) - 1) {
        leaf[n] = last[n];
        n++;
    }
    leaf[n] = '\0';

    /* walk to parent of last segment */
    it = p;
    tmpfs_inode_t* parent = NULL;
    cur = root;
    while (*it) {
        while (*it && tmpfs_is_sep(*it)) {
            it++;
        }
        if (!*it) {
            break;
        }
        const char* start = it;
        while (*it && !tmpfs_is_sep(*it)) {
            it++;
        }
        size_t seglen = (size_t) (it - start);
        if (seglen == flopstrlen(leaf) && flopstrncmp(start, leaf, seglen) == 0 && !*it) {
            break;
        }
        char seg[VFS_MAX_FILE_NAME];
        size_t m = seglen;
        if (m >= sizeof(seg)) {
            m = sizeof(seg) - 1;
        }
        flop_memcpy(seg, start, m);
        seg[m] = '\0';
        parent = cur;
        tmpfs_inode_t* nxt = tmpfs_find_child(cur, seg);
        if (!nxt) {
            return NULL;
        }
        cur = nxt;
    }
    flopstrcopy(leaf_out, leaf, flopstrlen(leaf) + 1);
    return parent ? parent : root;
}

static void tmpfs_free_tree(tmpfs_inode_t* n) {
    if (!n) {
        return;
    }
    tmpfs_dirent_t* d = n->children;
    while (d) {
        tmpfs_dirent_t* nx = d->next;
        tmpfs_free_tree(d->child);
        kfree(d, sizeof(*d));
        d = nx;
    }
    if (n->type == VFS_FILE) {
        tmpfs_free_node_pages(n);
    }
    kfree(n, sizeof(*n));
}

/* VFS ops */

static void* tmpfs_mount(char* device, char* mount_point, int type) {
    (void) device;
    (void) mount_point;
    (void) type;
    tmpfs_super_t* sb = (tmpfs_super_t*) kmalloc(sizeof(tmpfs_super_t));
    if (!sb) {
        return NULL;
    }

    refcount_init(&sb->refcount);
    static spinlock_t sb_initializer = SPINLOCK_INIT;
    sb->lock = sb_initializer;
    spinlock_init(&sb->lock);

    sb->root = tmpfs_inode_new("/", VFS_DIR);
    if (!sb->root) {
        kfree(sb, sizeof(*sb));
        return NULL;
    }
    static spinlock_t sb_root_initializer = SPINLOCK_INIT;
    sb->root->lock = sb_root_initializer;
    spinlock_init(&sb->root->lock);
    refcount_inc_not_zero(&sb->root->refcount);

    return sb;
}

static int tmpfs_unmount(struct vfs_mountpoint* mp, char* device) {
    (void) device;
    tmpfs_super_t* sb = (tmpfs_super_t*) mp->data_pointer;
    if (!sb) {
        return -1;
    }

    bool interrupt_state = spinlock(&sb->lock);

    if (refcount_dec_and_test(&sb->refcount)) {
        tmpfs_free_tree(sb->root);
        mp->data_pointer = NULL;
        spinlock_unlock(&sb->lock, interrupt_state);
        kfree(sb, sizeof(*sb));
        return 0;
    }

    spinlock_unlock(&sb->lock, interrupt_state);
    return 0;
}

static struct vfs_node* tmpfs_open(struct vfs_node* node, char* relpath) {
    tmpfs_super_t* sb = (tmpfs_super_t*) node->mountpoint->data_pointer;
    if (!sb) {
        return NULL;
    }
    bool super_block_interrupt_state = spinlock(&sb->lock);

    tmpfs_inode_t* target_inode = NULL;

    if (!relpath || relpath[0] == '\0' || (relpath[0] == '/' && relpath[1] == '\0')) {
        target_inode = sb->root;
    } else {
        const char* p = relpath;
        if (*p == '/') {
            p++;
        }
        tmpfs_inode_t* cur = sb->root;
        char seg[VFS_MAX_FILE_NAME];
        size_t i = 0;
        while (*p) {
            while (*p && tmpfs_is_sep(*p)) {
                p++;
            }
            if (!*p) {
                break;
            }
            i = 0;
            while (p[i] && !tmpfs_is_sep(p[i]) && i < sizeof(seg) - 1) {
                seg[i] = p[i];
                i++;
            }
            seg[i] = '\0';
            tmpfs_inode_t* nxt = tmpfs_find_child(cur, seg);
            if (!nxt) {
                target_inode = NULL;
                break;
            }
            cur = nxt;
            p += i;
            if (!*p) {
                target_inode = cur;
                break;
            }
        }
    }
    if (!target_inode) {
        spinlock_unlock(&sb->lock, super_block_interrupt_state);
        return NULL;
    }

    spinlock(&target_inode->lock);
    refcount_inc_not_zero(&target_inode->refcount);
    spinlock_unlock(&target_inode->lock, true);
    spinlock_unlock(&sb->lock, super_block_interrupt_state);

    tmpfs_handle_t* h = (tmpfs_handle_t*) kmalloc(sizeof(tmpfs_handle_t));
    if (!h) {
        spinlock(&target_inode->lock);
        refcount_dec_and_test(&target_inode->refcount);
        spinlock_unlock(&target_inode->lock, true);
        return NULL;
    }
    h->inode = target_inode;
    h->mode = node->vfs_mode;
    h->pos = 0;

    if ((node->vfs_mode & VFS_MODE_TRUNCATE) == VFS_MODE_TRUNCATE && target_inode->type == VFS_FILE) {
        spinlock(&target_inode->lock);
        tmpfs_free_node_pages(target_inode);
        spinlock_unlock(&target_inode->lock, true);
    }
    if ((node->vfs_mode & VFS_MODE_APPEND) == VFS_MODE_APPEND && target_inode->type == VFS_FILE) {
        h->pos = target_inode->size;
    }

    node->data_pointer = h;
    return node;
}

static int tmpfs_close(struct vfs_node* node) {
    if (!node || !node->data_pointer) {
        return -1;
    }
    tmpfs_handle_t* h = (tmpfs_handle_t*) node->data_pointer;

    spinlock(&h->inode->lock);
    if (refcount_dec_and_test(&h->inode->refcount)) {
        if (h->inode->type == VFS_FILE) {
            tmpfs_free_node_pages(h->inode);
        }
        kfree(h->inode, sizeof(*h->inode));
    }
    spinlock_unlock(&h->inode->lock, true);

    kfree(h, sizeof(*h));
    node->data_pointer = NULL;
    return 0;
}

static int tmpfs_seek(struct vfs_node* node, unsigned long offset, unsigned char whence) {
    if (!node || !node->data_pointer) {
        return -1;
    }
    tmpfs_handle_t* h = (tmpfs_handle_t*) node->data_pointer;
    size_t base = 0;
    if (whence == VFS_SEEK_STRT) {
        base = 0;
    } else if (whence == VFS_SEEK_CUR) {
        base = h->pos;
    } else if (whence == VFS_SEEK_END) {
        base = h->inode->size;
    } else {
        return -1;
    }
    h->pos = base + offset;
    return 0;
}

static int tmpfs_read(struct vfs_node* node, unsigned char* buffer, unsigned long size) {
    if (!node || !node->data_pointer) {
        return -1;
    }
    tmpfs_handle_t* h = (tmpfs_handle_t*) node->data_pointer;
    tmpfs_inode_t* f = h->inode;

    spinlock(&f->lock);
    if (f->type != VFS_FILE) {
        spinlock_unlock(&f->lock, true);
        return -1;
    }
    if (h->pos >= f->size) {
        spinlock_unlock(&f->lock, true);
        return 0;
    }
    size_t remaining = f->size - h->pos;
    size_t to_read = size < remaining ? size : remaining;

    size_t off = h->pos;
    size_t done = 0;
    while (done < to_read) {
        size_t pg_idx = off / PAGE_SIZE;
        size_t pg_off = off % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - pg_off;
        size_t left = to_read - done;
        if (chunk > left) {
            chunk = left;
        }

        if (pg_idx >= f->page_count || !f->pages[pg_idx]) {
            /* hole (shouldn't happen if size is accurate), treat as zeros */
            flop_memset((uint8_t*) buffer + done, 0, chunk);
        } else {
            flop_memcpy((uint8_t*) buffer + done, (uint8_t*) f->pages[pg_idx] + pg_off, chunk);
        }

        done += chunk;
        off += chunk;
    }

    h->pos += to_read;

    spinlock_unlock(&f->lock, true);
    return (int) to_read;
}

static int tmpfs_write(struct vfs_node* node, unsigned char* buffer, unsigned long size) {
    if (!node || !node->data_pointer) {
        return -1;
    }
    tmpfs_handle_t* h = (tmpfs_handle_t*) node->data_pointer;
    tmpfs_inode_t* f = h->inode;

    spinlock(&f->lock);
    if (f->type != VFS_FILE) {
        spinlock_unlock(&f->lock, true);
        return -1;
    }

    size_t endpos = h->pos + size;

    size_t need_pages = tmpfs_ceil_div(endpos, PAGE_SIZE);
    if (need_pages > f->page_count) {
        if (tmpfs_resize_pages(f, need_pages) != 0) {
            return -1;
        }
    }

    if (h->pos > f->size) {
        size_t z = f->size;
        while (z < h->pos) {
            size_t pg_idx = z / PAGE_SIZE;
            size_t pg_off = z % PAGE_SIZE;
            size_t chunk = PAGE_SIZE - pg_off;
            size_t left = h->pos - z;
            if (chunk > left) {
                chunk = left;
            }
            if (pg_idx < f->page_count && f->pages[pg_idx]) {
                flop_memset((uint8_t*) f->pages[pg_idx] + pg_off, 0, chunk);
            }
            z += chunk;
        }
    }

    /* perform the write across pages */
    size_t off = h->pos;
    size_t done = 0;
    while (done < size) {
        size_t pg_idx = off / PAGE_SIZE;
        size_t pg_off = off % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - pg_off;
        size_t left = size - done;
        if (chunk > left) {
            chunk = left;
        }

        if (pg_idx >= f->page_count || !f->pages[pg_idx]) {
            /* allocate missing page, shouldn't happen after resize, but always use protection :^) */
            if (tmpfs_resize_pages(f, pg_idx + 1) != 0) {
                return -1;
            }
        }

        flop_memcpy((uint8_t*) f->pages[pg_idx] + pg_off, (uint8_t*) buffer + done, chunk);

        done += chunk;
        off += chunk;
    }

    h->pos += size;
    if (f->size < h->pos) {
        f->size = h->pos;
    }

    spinlock_unlock(&f->lock, true);
    return (int) size;
}

static int tmpfs_create(struct vfs_mountpoint* mp, char* relpath) {
    tmpfs_super_t* sb = (tmpfs_super_t*) mp->data_pointer;
    if (!relpath || relpath[0] == '\0' || flopstrcmp(relpath, "/") == 0) {
        return -1;
    }

    bool sb_ints = spinlock(&sb->lock);
    char leaf[VFS_MAX_FILE_NAME];
    tmpfs_inode_t* parent = tmpfs_parent_and_leaf(sb->root, relpath, leaf);
    if (!parent || parent->type != VFS_DIR) {
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }

    bool parent_ints = spinlock(&parent->lock);
    if (tmpfs_find_child(parent, leaf)) {
        spinlock_unlock(&parent->lock, parent_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return 0;
    }

    tmpfs_inode_t* f = tmpfs_inode_new(leaf, VFS_FILE);
    if (!f) {
        spinlock_unlock(&parent->lock, parent_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }
    f->parent = parent;
    if (!tmpfs_dirent_prepend(parent, f)) {
        kfree(f, sizeof(*f));
        spinlock_unlock(&parent->lock, parent_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }
    spinlock_unlock(&parent->lock, parent_ints);
    spinlock_unlock(&sb->lock, sb_ints);
    return 0;
}

static int tmpfs_delete(struct vfs_mountpoint* mp, char* relpath) {
    tmpfs_super_t* sb = (tmpfs_super_t*) mp->data_pointer;
    if (!relpath || !relpath[0] || flopstrcmp(relpath, "/") == 0) {
        return -1;
    }

    bool sb_ints = spinlock(&sb->lock);
    char leaf[VFS_MAX_FILE_NAME];
    tmpfs_inode_t* parent = tmpfs_parent_and_leaf(sb->root, relpath, leaf);
    if (!parent) {
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }

    bool parent_ints = spinlock(&parent->lock);
    tmpfs_inode_t* target_inode = tmpfs_find_child(parent, leaf);
    if (!target_inode) {
        spinlock_unlock(&parent->lock, parent_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }

    bool t_ints = spinlock(&target_inode->lock);
    if (target_inode->type == VFS_DIR && target_inode->children) {
        spinlock_unlock(&target_inode->lock, t_ints);
        spinlock_unlock(&parent->lock, parent_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }
    tmpfs_dirent_remove(parent, target_inode);
    if (target_inode->type == VFS_FILE) {
        tmpfs_free_node_pages(target_inode);
    }
    kfree(target_inode, sizeof(*target_inode));
    spinlock_unlock(&target_inode->lock, t_ints);
    spinlock_unlock(&parent->lock, parent_ints);
    spinlock_unlock(&sb->lock, sb_ints);
    return 0;
}

static int tmpfs_rename(struct vfs_mountpoint* mp, char* oldp, char* newp) {
    tmpfs_super_t* sb = (tmpfs_super_t*) mp->data_pointer;
    if (!oldp || !newp) {
        return -1;
    }

    bool sb_ints = spinlock(&sb->lock);

    char oldleaf[VFS_MAX_FILE_NAME];
    tmpfs_inode_t* oldpar = tmpfs_parent_and_leaf(sb->root, oldp, oldleaf);
    if (!oldpar) {
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }

    bool oldpar_ints = spinlock(&oldpar->lock);
    tmpfs_inode_t* node = tmpfs_find_child(oldpar, oldleaf);
    if (!node) {
        spinlock_unlock(&oldpar->lock, oldpar_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }

    char newleaf[VFS_MAX_FILE_NAME];
    tmpfs_inode_t* newpar = tmpfs_parent_and_leaf(sb->root, newp, newleaf);
    if (!newpar || newpar->type != VFS_DIR) {
        spinlock_unlock(&oldpar->lock, oldpar_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }

    bool newpar_ints = spinlock(&newpar->lock);
    if (tmpfs_find_child(newpar, newleaf)) {
        spinlock_unlock(&newpar->lock, newpar_ints);
        spinlock_unlock(&oldpar->lock, oldpar_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return -1;
    }

    tmpfs_dirent_remove(oldpar, node);
    node->parent = newpar;
    flopstrcopy(node->name, newleaf, flopstrlen(newleaf) + 1);
    tmpfs_dirent_prepend(newpar, node);

    spinlock_unlock(&newpar->lock, newpar_ints);
    spinlock_unlock(&oldpar->lock, oldpar_ints);
    spinlock_unlock(&sb->lock, sb_ints);
    return 0;
}

static struct vfs_directory_list* tmpfs_listdir(struct vfs_mountpoint* mp, char* relpath) {
    tmpfs_super_t* sb = (tmpfs_super_t*) mp->data_pointer;
    bool sb_ints = spinlock(&sb->lock);

    tmpfs_inode_t* dir = sb->root;
    if (relpath && flopstrcmp(relpath, "/") != 0) {
        const char* p = relpath;
        if (*p == '/') {
            p++;
        }
        char seg[VFS_MAX_FILE_NAME];
        size_t i = 0;
        tmpfs_inode_t* cur = sb->root;
        while (*p) {
            while (*p && tmpfs_is_sep(*p)) {
                p++;
            }
            if (!*p) {
                break;
            }
            i = 0;
            while (p[i] && !tmpfs_is_sep(p[i]) && i < sizeof(seg) - 1) {
                seg[i] = p[i];
                i++;
            }
            seg[i] = '\0';
            cur = tmpfs_find_child(cur, seg);
            if (!cur) {
                spinlock_unlock(&sb->lock, sb_ints);
                return NULL;
            }
            p += i;
        }
        dir = cur;
    }

    bool d_ints = spinlock(&dir->lock);
    if (!dir || dir->type != VFS_DIR) {
        spinlock_unlock(&dir->lock, d_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return NULL;
    }

    struct vfs_directory_list* list = (struct vfs_directory_list*) kmalloc(sizeof(struct vfs_directory_list));
    if (!list) {
        spinlock_unlock(&dir->lock, d_ints);
        spinlock_unlock(&sb->lock, sb_ints);
        return NULL;
    }

    list->head = list->tail = NULL;
    tmpfs_dirent_t* d = dir->children;
    while (d) {
        struct vfs_directory_entry* e = (struct vfs_directory_entry*) kmalloc(sizeof(struct vfs_directory_entry));
        if (!e) {
            break;
        }
        flop_memset(e, 0, sizeof(*e));
        flopstrcopy(e->name, d->child->name, flopstrlen(d->child->name) + 1);
        e->type = d->child->type;
        if (!list->head) {
            list->head = e;
        } else {
            list->tail->next = e;
        }
        list->tail = e;
        d = d->next;
    }

    spinlock_unlock(&dir->lock, d_ints);
    spinlock_unlock(&sb->lock, sb_ints);
    return list;
}

static int tmpfs_ctrl(struct vfs_node* node, unsigned long cmd, unsigned long arg) {
    if (!node || !node->data_pointer) {
        return -1;
    }
    tmpfs_handle_t* h = (tmpfs_handle_t*) node->data_pointer;
    tmpfs_inode_t* f = h->inode;
    if (!f) {
        return -1;
    }

    bool ints = spinlock(&f->lock);
    int ret = -1;

    switch (cmd) {
        case TMPFS_CMD_GET_SIZE:
            if (arg) {
                *(size_t*) arg = f->size;
                ret = 0;
            }
            break;
        case TMPFS_CMD_SET_SIZE: {
            size_t new_size = (size_t) arg;
            size_t need_pages = tmpfs_ceil_div(new_size, PAGE_SIZE);
            if (need_pages > f->page_count) {
                if (tmpfs_resize_pages(f, need_pages) == 0) {
                    f->size = new_size;
                    ret = 0;
                }
            } else {
                for (size_t i = need_pages; i < f->page_count; i++) {
                    if (f->pages[i]) {
                        kfree(f->pages[i], PAGE_SIZE);
                        f->pages[i] = NULL;
                    }
                }
                f->page_count = need_pages;
                f->size = new_size;
                ret = 0;
            }
            if (h->pos > new_size) {
                h->pos = new_size;
            }
            break;
        }
        case TMPFS_CMD_TRUNCATE: {
            size_t new_size = (size_t) arg;
            if (new_size < f->size) {
                size_t need_pages = tmpfs_ceil_div(new_size, PAGE_SIZE);
                for (size_t i = need_pages; i < f->page_count; i++) {
                    if (f->pages[i]) {
                        kfree(f->pages[i], PAGE_SIZE);
                        f->pages[i] = NULL;
                    }
                }
                f->page_count = need_pages;
                f->size = new_size;
                if (h->pos > new_size) {
                    h->pos = new_size;
                }
            }
            ret = 0;
            break;
        }
        case TMPFS_CMD_SYNC:
            ret = 0;
            break;
    }

    spinlock_unlock(&f->lock, ints);
    return ret;
}

// we only expose the tmpfs through the vfs op table

static int tmpfs_init_op_table(int fs_type) {
    flop_memset(&tmpfs_fs, 0, sizeof(tmpfs_fs));
    tmpfs_fs.filesystem_type = fs_type;
    tmpfs_fs.op_table.open = tmpfs_open;
    tmpfs_fs.op_table.close = tmpfs_close;
    tmpfs_fs.op_table.read = tmpfs_read;
    tmpfs_fs.op_table.write = tmpfs_write;
    tmpfs_fs.op_table.mount = tmpfs_mount;
    tmpfs_fs.op_table.unmount = tmpfs_unmount;
    tmpfs_fs.op_table.create = tmpfs_create;
    tmpfs_fs.op_table.delete = tmpfs_delete;
    tmpfs_fs.op_table.rename = tmpfs_rename;
    tmpfs_fs.op_table.ctrl = tmpfs_ctrl;
    tmpfs_fs.op_table.seek = tmpfs_seek;
    tmpfs_fs.op_table.listdir = tmpfs_listdir;
    return 0;
}

int tmpfs_register_with_vfs() {
    int fs_type = VFS_TYPE_TMPFS;
    tmpfs_init_op_table(fs_type);
    int ret = vfs_mount("tmpfs", "/tmp/", fs_type);

    if (ret < 0) {
        log("tmpfs: failed to mount tmpfs", RED);
        return -1;
    }
    return vfs_acknowledge_fs(&tmpfs_fs);
}

int tmpfs_init() {
    int ret = tmpfs_register_with_vfs();
    if (ret < 0) {
        log("tmpfs: failed to register with VFS\n", RED);
        return ret;
    }
    log("tmpfs: init - ok\n", GREEN);
    return 0;
}
