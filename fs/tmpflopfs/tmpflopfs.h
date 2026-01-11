#ifndef TMPFLOPFS_H
#define TMPFLOPFS_H

#include <stddef.h>
#include <stdint.h>
#include "../vfs/vfs.h"

typedef enum tmpfs_node_type {
    TMPFS_NODE_FILE = VFS_FILE,
    TMPFS_NODE_DIR = VFS_DIR,
    TMPFS_NODE_DEV = VFS_DEV,
    TMPFS_NODE_SYMLINK = VFS_SYMLINK,
    TMPFS_NODE_PIPE = VFS_PIPE
} tmpfs_node_type_t;

struct tmpfs_node {
    char name[VFS_MAX_FILE_NAME];
    tmpfs_node_type_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t offset;
    uint32_t nlink;
    uint32_t ino;

    void** pages;
    uint32_t page_count;

    struct tmpfs_node* parent;
    struct tmpfs_node* children;
    struct tmpfs_node* next_sibling;
};

void tmpfs_init(void);

#endif
