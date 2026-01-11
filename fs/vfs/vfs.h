#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "../../lib/refcount.h"
#include "../../task/ipc/pipe.h"
#define VFS_MAX_FILE_NAME 256

#define VFS_FILE 0x0
#define VFS_DIR 0x1
#define VFS_DEV 0x2
#define VFS_SYMLINK 0x3
#define VFS_HIDDEN 0x4
#define VFS_PIPE 0x5

typedef enum vfs_mode {
    VFS_MODE_R = 0x1,
    VFS_MODE_W = 0x2,
    VFS_MODE_RW = (VFS_MODE_R | VFS_MODE_W),
    VFS_MODE_CREATE = 0x4,
    VFS_MODE_TRUNCATE = 0x8,
    VFS_MODE_APPEND = 0x9
} vfs_mode_t;

typedef enum vfs_seek {
    VFS_SEEK_STRT = 0x0,
    VFS_SEEK_CUR = 0x1,
    VFS_SEEK_END = 0x2
} vfs_seek_t;

typedef enum vfs_fs_type {
    VFS_FS_TMPFS = 0x1,
    VFS_FS_FAT = 0x2,
    VFS_FS_DEVFS = 0x3,
    VFS_FS_PROCFS = 0x4
} vfs_fs_type_t;

struct vfs_mountpoint {
    struct vfs_fs* filesystem;
    struct vfs_mountpoint* next_mountpoint;
    char* mount_point;
    char* device_name;
    void* data_pointer;
    refcount_t refcount;
};

struct vfs_directory_entry {
    char name[VFS_MAX_FILE_NAME];
    int type;
    struct vfs_directory_entry* next;
};

struct vfs_directory_list {
    struct vfs_directory_entry* head;
    struct vfs_directory_entry* tail;
};

typedef struct stat {
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
    uint32_t st_nlink;
    uint32_t st_ino;
    uint32_t st_dev;
} stat_t;

struct vfs_fs_list {
    struct vfs_fs* head;
};

struct vfs_mp_list {
    struct vfs_mountpoint* head;
    struct vfs_mountpoint* tail;
    spinlock_t lock;
};

struct vfs_node {
    pipe_t pipe;
    struct vfs_mountpoint* mountpoint;
    void* data_pointer;
    vfs_mode_t vfs_mode;
    refcount_t refcount;
    stat_t stat;
    struct vfs_op_tbl* ops;
    char* name;
};

struct vfs_file_descriptor {
    struct vfs_node* node;
    pipe_t* pipe;
    refcount_t refcount;
};

typedef int (*rw)(struct vfs_node*, unsigned char*, unsigned long);

struct vfs_op_tbl {
    struct vfs_node* (*open)(struct vfs_node*, char*);
    int (*close)(struct vfs_node*);
    rw read;
    rw write;
    void* (*mount)(char*, char*, int);
    int (*unmount)(struct vfs_mountpoint*, char*);
    int (*create)(struct vfs_mountpoint*, char*);
    int (*delete)(struct vfs_mountpoint*, char*);
    int (*unlink)(struct vfs_mountpoint*, char*);
    int (*mkdir)(struct vfs_mountpoint*, char*, uint32_t);
    int (*rmdir)(struct vfs_mountpoint*, char*);
    int (*rename)(struct vfs_mountpoint*, char*, char*);
    int (*ctrl)(struct vfs_node*, unsigned long, unsigned long);
    int (*seek)(struct vfs_node*, unsigned long, unsigned char);
    struct vfs_directory_list* (*listdir)(struct vfs_mountpoint*, char*);
    int (*stat)(const char* path, struct stat* st);
    int (*fstat)(struct vfs_node* node, struct stat* st);
    int (*lstat)(const char* path, struct stat* st);
    int (*truncate)(struct vfs_node*, uint64_t length);
    int (*ioctl)(struct vfs_node* node, unsigned long cmd, unsigned long arg);
    int (*link)(struct vfs_mountpoint*, char*, char*);
};

struct vfs_fs {
    struct vfs_op_tbl op_table;
    vfs_fs_type_t filesystem_type;
    struct vfs_fs* previous;
    char* name;
    char* mount_point;
};

int vfs_init(void);
int vfs_acknowledge_fs(struct vfs_fs* fs);
int vfs_unacknowledge_fs(struct vfs_fs* fs);
int vfs_mount(char* device, char* mount_point, int type);
int vfs_unmount(char* mount_point);
struct vfs_node* vfs_open(char* name, int mode);
int vfs_close(struct vfs_node* node);
int vfs_read(struct vfs_node* node, unsigned char* buffer, unsigned long size);
int vfs_write(struct vfs_node* node, unsigned char* buffer, unsigned long size);
struct vfs_directory_list* vfs_listdir(struct vfs_mountpoint* mp, char* path);
int vfs_ctrl(struct vfs_node* node, unsigned long command, unsigned long arg);
int vfs_seek(struct vfs_node* node, unsigned long offset, unsigned char whence);
int vfs_stat(char* path, stat_t* st);
int vfs_fstat(struct vfs_node* node, stat_t* st);
int vfs_truncate(struct vfs_node* node, uint32_t new_size);
int vfs_ftruncate(struct vfs_node* node, uint32_t len);
int vfs_truncate_path(char* path, uint64_t length);
int vfs_unlink(char* path);
int vfs_link(char* oldpath, char* newpath);
int vfs_mkdir(char* path, uint32_t mode);
int vfs_rmdir(char* path);
int vfs_rename(char* oldpath, char* newpath);
struct vfs_directory_list* vfs_readdir_path(char* path);
int vfs_ioctl(struct vfs_node* node, unsigned long cmd, unsigned long arg);

#endif
