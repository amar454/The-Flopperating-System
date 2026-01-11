#include "tmpflopfs.h"
#include "../vfs/vfs.h"
#include "../../lib/logging.h"
#include "../../lib/refcount.h"
#include "../../mem/alloc.h"
#include "../../mem/paging.h"
#include "../../mem/utils.h"
#include "../../mem/vmm.h"
#include "../../mem/pmm.h"
#include "../../drivers/vga/vgahandler.h"
#include "../../lib/str.h"
#include "../../task/sync/spinlock.h"
#include <stddef.h>
#include <stdint.h>

static struct vfs_fs tmpflopfs;

static struct tmpfs_node* tmpfs_node_internal_create(const char* name, tmpfs_node_type_t type) {
    struct tmpfs_node* node = kmalloc(sizeof(struct tmpfs_node));
    if (!node) {
        return NULL;
    }
    flop_memset(node, 0, sizeof(struct tmpfs_node));
    flopstrcopy(node->name, (char*) name, flopstrlen((char*) name) + 1);
    node->type = type;
    node->mode = 0777;
    return node;
}

static struct tmpfs_node* tmpfs_walk_path(struct tmpfs_node* root, const char* path) {
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return root;
    }

    char tmp[VFS_MAX_FILE_NAME];
    flopstrcopy(tmp, (char*) path, flopstrlen(path) + 1);

    char* segment = tmp;
    if (*segment == '/') {
        segment++;
    }

    struct tmpfs_node* curr = root;

    while (*segment) {
        char* next_slash = NULL;
        for (char* p = segment; *p; p++) {
            if (*p == '/') {
                next_slash = p;
                *p = '\0';
                break;
            }
        }

        struct tmpfs_node* child = curr->children;
        struct tmpfs_node* found = NULL;
        while (child) {
            if (flopstrcmp(child->name, segment) == 0) {
                found = child;
                break;
            }
            child = child->next_sibling;
        }

        if (!found) {
            return NULL;
        }

        curr = found;
        if (!next_slash) {
            break;
        }
        segment = next_slash + 1;
    }

    return curr;
}

void* tmpfs_op_mount(char* device, char* mount_point, int type) {
    return tmpfs_node_internal_create("/", TMPFS_NODE_DIR);
}

int tmpfs_op_unmount(struct vfs_mountpoint* mp, char* path) {
    struct tmpfs_node* root = (struct tmpfs_node*) mp->data_pointer;
    if (root) {
        kfree(root, sizeof(struct tmpfs_node));
    }
    return 0;
}

struct vfs_node* tmpfs_op_open(struct vfs_node* node, char* name) {
    struct tmpfs_node* root = (struct tmpfs_node*) node->mountpoint->data_pointer;
    struct tmpfs_node* target = tmpfs_walk_path(root, name);
    if (!target) {
        return NULL;
    }
    node->data_pointer = target;
    node->stat.st_size = target->size;
    node->stat.st_mode = target->mode;
    node->stat.st_uid = target->uid;
    node->stat.st_gid = target->gid;
    return node;
}

int tmpfs_op_close(struct vfs_node* node) {
    return 0;
}

int tmpfs_op_read(struct vfs_node* node, unsigned char* buffer, unsigned long size) {
    struct tmpfs_node* t = (struct tmpfs_node*) node->data_pointer;
    if (t->offset >= t->size) {
        return 0;
    }
    if (t->offset + size > t->size) {
        size = t->size - t->offset;
    }
    unsigned long read_total = 0;
    while (read_total < size) {
        uint32_t p_idx = t->offset / PAGE_SIZE;
        uint32_t p_off = t->offset % PAGE_SIZE;
        uint32_t chunk = PAGE_SIZE - p_off;
        if (chunk > (size - read_total)) {
            chunk = size - read_total;
        }
        flop_memcpy(buffer + read_total, (uint8_t*) t->pages[p_idx] + p_off, chunk);
        read_total += chunk;
        t->offset += chunk;
    }
    return read_total;
}

int tmpfs_op_write(struct vfs_node* node, unsigned char* buffer, unsigned long size) {
    struct tmpfs_node* t = (struct tmpfs_node*) node->data_pointer;
    unsigned long end = t->offset + size;
    uint32_t needed = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    if (needed > t->page_count) {
        void** new_pages = kmalloc(needed * sizeof(void*));
        if (t->pages) {
            flop_memcpy(new_pages, t->pages, t->page_count * sizeof(void*));
            kfree(t->pages, t->page_count * sizeof(void*));
        }
        for (uint32_t i = t->page_count; i < needed; i++) {
            new_pages[i] = pmm_alloc_pages(0, 1);
            flop_memset(new_pages[i], 0, PAGE_SIZE);
        }
        t->pages = new_pages;
        t->page_count = needed;
    }
    unsigned long written = 0;
    while (written < size) {
        uint32_t p_idx = t->offset / PAGE_SIZE;
        uint32_t p_off = t->offset % PAGE_SIZE;
        uint32_t chunk = PAGE_SIZE - p_off;
        if (chunk > (size - written)) {
            chunk = size - written;
        }
        flop_memcpy((uint8_t*) t->pages[p_idx] + p_off, buffer + written, chunk);
        written += chunk;
        t->offset += chunk;
    }
    if (t->offset > t->size) {
        t->size = t->offset;
    }
    node->stat.st_size = t->size;
    return written;
}

int tmpfs_op_seek(struct vfs_node* node, unsigned long offset, unsigned char whence) {
    struct tmpfs_node* t = (struct tmpfs_node*) node->data_pointer;
    if (whence == VFS_SEEK_STRT) {
        t->offset = offset;
    } else if (whence == VFS_SEEK_CUR) {
        t->offset += offset;
    } else if (whence == VFS_SEEK_END) {
        t->offset = t->size + offset;
    }
    return 0;
}

int tmpfs_op_truncate(struct vfs_node* node, uint64_t length) {
    struct tmpfs_node* t = (struct tmpfs_node*) node->data_pointer;
    uint32_t needed = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (needed < t->page_count) {
        for (uint32_t i = needed; i < t->page_count; i++) {
            pmm_free_pages(t->pages[i], 0, 1);
        }
    }
    t->size = length;
    t->page_count = needed;
    node->stat.st_size = length;
    return 0;
}

int tmpfs_op_create(struct vfs_mountpoint* mp, char* name) {
    struct tmpfs_node* root = (struct tmpfs_node*) mp->data_pointer;
    char dir_path[VFS_MAX_FILE_NAME];
    char file_name[VFS_MAX_FILE_NAME];
    int last_slash = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '/') {
            last_slash = i;
        }
    }

    struct tmpfs_node* parent = root;
    if (last_slash != -1) {
        flop_memcpy(dir_path, name, last_slash);
        dir_path[last_slash] = '\0';
        parent = tmpfs_walk_path(root, dir_path);
        flopstrcopy(file_name, name + last_slash + 1, flopstrlen(name + last_slash + 1) + 1);
    } else {
        flopstrcopy(file_name, name, flopstrlen(name) + 1);
    }

    if (!parent) {
        return -1;
    }
    struct tmpfs_node* n = tmpfs_node_internal_create(file_name, TMPFS_NODE_FILE);
    n->parent = parent;
    n->next_sibling = parent->children;
    parent->children = n;
    return 0;
}

int tmpfs_op_mkdir(struct vfs_mountpoint* mp, char* name, uint32_t mode) {
    struct tmpfs_node* root = (struct tmpfs_node*) mp->data_pointer;
    char dir_path[VFS_MAX_FILE_NAME];
    char new_dir_name[VFS_MAX_FILE_NAME];
    int last_slash = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '/') {
            last_slash = i;
        }
    }

    struct tmpfs_node* parent = root;
    if (last_slash != -1) {
        flop_memcpy(dir_path, name, last_slash);
        dir_path[last_slash] = '\0';
        parent = tmpfs_walk_path(root, dir_path);
        flopstrcopy(new_dir_name, name + last_slash + 1, flopstrlen(name + last_slash + 1) + 1);
    } else {
        flopstrcopy(new_dir_name, name, flopstrlen(name) + 1);
    }

    if (!parent) {
        return -1;
    }
    struct tmpfs_node* n = tmpfs_node_internal_create(new_dir_name, TMPFS_NODE_DIR);
    n->mode = mode;
    n->parent = parent;
    n->next_sibling = parent->children;
    parent->children = n;
    return 0;
}

int tmpfs_op_unlink(struct vfs_mountpoint* mp, char* name) {
    struct tmpfs_node* root = (struct tmpfs_node*) mp->data_pointer;
    struct tmpfs_node* target = tmpfs_walk_path(root, name);
    if (!target || !target->parent) {
        return -1;
    }

    struct tmpfs_node* parent = target->parent;
    struct tmpfs_node* curr = parent->children;
    struct tmpfs_node* prev = NULL;

    while (curr) {
        if (curr == target) {
            if (prev) {
                prev->next_sibling = curr->next_sibling;
            } else {
                parent->children = curr->next_sibling;
            }
            for (uint32_t i = 0; i < curr->page_count; i++) {
                pmm_free_pages(curr->pages[i], 0, 1);
            }
            if (curr->pages) {
                kfree(curr->pages, curr->page_count * sizeof(void*));
            }
            kfree(curr, sizeof(struct tmpfs_node));
            return 0;
        }
        prev = curr;
        curr = curr->next_sibling;
    }
    return -1;
}

int tmpfs_op_rename(struct vfs_mountpoint* mp, char* old, char* new) {
    struct tmpfs_node* root = (struct tmpfs_node*) mp->data_pointer;
    struct tmpfs_node* target = tmpfs_walk_path(root, old);
    if (!target) {
        return -1;
    }
    flopstrcopy(target->name, new, flopstrlen(new) + 1);
    return 0;
}

int tmpfs_op_fstat(struct vfs_node* node, struct stat* st) {
    struct tmpfs_node* t = (struct tmpfs_node*) node->data_pointer;
    st->st_size = t->size;
    st->st_mode = t->mode;
    st->st_uid = t->uid;
    st->st_gid = t->gid;
    st->st_nlink = t->nlink;
    st->st_ino = t->ino;
    return 0;
}

struct vfs_directory_list* tmpfs_op_listdir(struct vfs_mountpoint* mp, char* path) {
    struct tmpfs_node* root = (struct tmpfs_node*) mp->data_pointer;
    struct tmpfs_node* dir = tmpfs_walk_path(root, path);
    if (!dir || dir->type != TMPFS_NODE_DIR) {
        return NULL;
    }

    struct vfs_directory_list* list = kmalloc(sizeof(struct vfs_directory_list));
    flop_memset(list, 0, sizeof(struct vfs_directory_list));

    struct tmpfs_node* curr = dir->children;
    while (curr) {
        struct vfs_directory_entry* entry = kmalloc(sizeof(struct vfs_directory_entry));
        flop_memset(entry, 0, sizeof(struct vfs_directory_entry));
        flopstrcopy(entry->name, curr->name, flopstrlen(curr->name) + 1);
        entry->type = curr->type;

        if (!list->head) {
            list->head = entry;
            list->tail = entry;
        } else {
            list->tail->next = entry;
            list->tail = entry;
        }
        curr = curr->next_sibling;
    }
    return list;
}

void tmpfs_init() {
    flop_memset(&tmpflopfs, 0, sizeof(struct vfs_fs));
    tmpflopfs.filesystem_type = VFS_FS_TMPFS;
    tmpflopfs.name = "tmpfs";
    tmpflopfs.op_table.mount = tmpfs_op_mount;
    tmpflopfs.op_table.unmount = tmpfs_op_unmount;
    tmpflopfs.op_table.open = tmpfs_op_open;
    tmpflopfs.op_table.close = tmpfs_op_close;
    tmpflopfs.op_table.read = tmpfs_op_read;
    tmpflopfs.op_table.write = tmpfs_op_write;
    tmpflopfs.op_table.seek = tmpfs_op_seek;
    tmpflopfs.op_table.truncate = tmpfs_op_truncate;
    tmpflopfs.op_table.create = tmpfs_op_create;
    tmpflopfs.op_table.mkdir = tmpfs_op_mkdir;
    tmpflopfs.op_table.unlink = tmpfs_op_unlink;
    tmpflopfs.op_table.rename = tmpfs_op_rename;
    tmpflopfs.op_table.fstat = tmpfs_op_fstat;
    tmpflopfs.op_table.listdir = tmpfs_op_listdir;
    vfs_acknowledge_fs(&tmpflopfs);
}
