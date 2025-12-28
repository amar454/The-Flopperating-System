#ifndef SYSCALL_H
#define SYSCALL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#pragma once
#include "../task/sync/spinlock.h"

struct syscall_args {
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t a4;
    uint32_t a5;
};

typedef int (*syscall_function_pointer)(struct syscall_args*);
int c_syscall_routine(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);

typedef enum syscall_num {
    SYSCALL_READ = 0,
    SYSCALL_WRITE = 1,
    SYSCALL_FORK = 2,
    SYSCALL_OPEN = 3,
    SYSCALL_CLOSE = 4,
    SYSCALL_MMAP = 5,
    SYSCALL_SEEK = 6,
    SYSCALL_STAT = 7,
    SYSCALL_FSTAT = 8,
    SYSCALL_UNLINK = 9,
    SYSCALL_MKDIR = 10,
    SYSCALL_RMDIR = 11,
    SYSCALL_TRUNCATE = 12,
    SYSCALL_FTRUNCATE = 13,
    SYSCALL_RENAME = 14,
    SYSCALL_GETPID = 15,
    SYSCALL_CHDIR = 16,
    SYSCALL_DUP = 17,
    SYSCALL_PIPE = 18,
    SYSCALL_CLONE = 19,
    SYSCALL_IOCTL = 20,
    SYSCALL_PRINT = 21,
    SYSCALL_REBOOT = 22,
    SYSCALL_MUNMAP = 23,
    SYSCALL_CREAT = 24,
    SYSCALL_SCHED_YIELD = 25,
    SYSCALL_KILL = 26,
    SYSCALL_LINK = 27,
    SYSCALL_GETUID = 28,
    SYSCALL_GETGID = 29,
    SYSCALL_GETEUID = 30,
    SYSCALL_GETSID = 31,
    SYSCALL_SETUID = 32,
    SYSCALL_SETGID = 33,
    SYSCALL_REGIDT = 34,
    SYSCALL_GET_PRIORITY_MAX = 35,
    SYSCALL_GET_PRIORITY_MIN = 36,
    SYSCALL_FSMOUNT = 37,
    SYSCALL_COPY_FILE_RANGE = 38,
    SYSCALL_GETCWD = 39,
    SYSCALL_MPROTECT = 40,
    SYSCALL_MREMAP = 41
} syscall_num_t;

typedef struct int_frame {
    uint32_t gs, fs, es, ds;

    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;

    uint32_t int_no;
    uint32_t err_code;

    uint32_t eip, cs, eflags, useresp, ss;
} int_frame_t;

typedef struct syscall_table {
    int (*sys_read)(struct syscall_args* args);
    int (*sys_write)(struct syscall_args* args);
    int (*sys_open)(struct syscall_args* args);
    int (*sys_close)(struct syscall_args* args);
    int (*sys_mmap)(struct syscall_args* args);
    int (*sys_seek)(struct syscall_args* args);
    int (*sys_stat)(struct syscall_args* args);
    int (*sys_fstat)(struct syscall_args* args);
    int (*sys_unlink)(struct syscall_args* args);
    int (*sys_mkdir)(struct syscall_args* args);
    int (*sys_rmdir)(struct syscall_args* args);
    int (*sys_truncate)(struct syscall_args* args);
    int (*sys_ftruncate)(struct syscall_args* args);
    int (*sys_rename)(struct syscall_args* args);
    int (*sys_print)(struct syscall_args* args);
    int (*sys_chdir)(struct syscall_args* args);
    int (*sys_dup)(struct syscall_args* args);
    int (*sys_pipe)(struct syscall_args* args);
    int (*sys_ioctl)(struct syscall_args* args);
    int (*sys_reboot)(struct syscall_args* args);
    int (*sys_munmap)(struct syscall_args* args);
    int (*sys_creat)(struct syscall_args* args);
    int (*sys_sched_yield)(struct syscall_args* args);
    int (*sys_kill)(struct syscall_args* args);
    int (*sys_link)(struct syscall_args* args);
    int (*sys_setuid)(struct syscall_args* args);
    int (*sys_setgid)(struct syscall_args* args);
    int (*sys_regidt)(struct syscall_args* args);
    int (*sys_get_priority_max)(struct syscall_args* args);
    int (*sys_get_priority_min)(struct syscall_args* args);
    int (*sys_fsmount)(struct syscall_args* args);
    int (*sys_copy_file_range)(struct syscall_args* args);
    int (*sys_mprotect)(struct syscall_args* args);
    int (*sys_mremap)(struct syscall_args* args);
    int (*sys_getcwd)(struct syscall_args* args);
    int (*sys_fork)(struct syscall_args* args);
    int (*sys_getuid)(struct syscall_args* args);
    int (*sys_getgid)(struct syscall_args* args);
    int (*sys_geteuid)(struct syscall_args* args);
    int (*sys_getpid)(struct syscall_args* args);
    int (*sys_clone)(struct syscall_args* args);
    int (*sys_getsid)(struct syscall_args* args);
} syscall_table_t;

int syscall(syscall_num_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);

// 0: read(fd, buf, count)
int sys_read(struct syscall_args* args);

// 1: write(fd, buf, count)
int sys_write(struct syscall_args* args);

// 2: fork()
int sys_fork(struct syscall_args* args);

// 3: open(path_ptr, flags)
int sys_open(struct syscall_args* args);

// 4: close(fd)
int sys_close(struct syscall_args* args);

// 5: mmap(addr, len, flags, fd, offset)
int sys_mmap(struct syscall_args* args);

// 6: seek(fd, offset, whence)
int sys_seek(struct syscall_args* args);

// 7: stat(path, st)
int sys_stat(struct syscall_args* args);

// 8: fstat(fd, st)
int sys_fstat(struct syscall_args* args);

// 9: unlink(path)
int sys_unlink(struct syscall_args* args);

// 10: mkdir(path, mode)
int sys_mkdir(struct syscall_args* args);

// 11: rmdir(path)
int sys_rmdir(struct syscall_args* args);

// 12: truncate(path, length)
int sys_truncate(struct syscall_args* args);

// 13: ftruncate(fd, length)
int sys_ftruncate(struct syscall_args* args);

// 14: rename(oldpath, newpath)
int sys_rename(struct syscall_args* args);

// 15: getpid()
int sys_getpid(struct syscall_args* args);

// 16: chdir(path)
int sys_chdir(struct syscall_args* args);

// 17: dup(pid)
int sys_dup(struct syscall_args* args);

// 18: pipe(pipefd[2])
int sys_pipe(struct syscall_args* args);

// 19: clone(flags, stack)
int sys_clone(struct syscall_args* args);

// 20: ioctl(fd, request, arg)
int sys_ioctl(struct syscall_args* args);

// 21: print(str_ptr)
int sys_print(struct syscall_args* args);

// 22: reboot()
int sys_reboot(struct syscall_args* args);

// 23: munmap(addr, len)
int sys_munmap(struct syscall_args* args);

// 24: creat(path, mode)
int sys_creat(struct syscall_args* args);

// 25: sched_yield()
int sys_sched_yield(struct syscall_args* args);

// 26: kill(pid)
int sys_kill(struct syscall_args* args);

// 27: link(oldpath, newpath)
int sys_link(struct syscall_args* args);

// 28: getuid()
int sys_getuid(struct syscall_args* args);

// 29: getgid()
int sys_getgid(struct syscall_args* args);

// 30: geteuid()
int sys_geteuid(struct syscall_args* args);

// 31: getsid()
int sys_getsid(struct syscall_args* args);

// 32: setuid(ruid, uid)
int sys_setuid(struct syscall_args* args);

// 33: setgid(rgid, gid)
int sys_setgid(struct syscall_args* args);

// 34: regidt(rgid, gid)
int sys_regidt(struct syscall_args* args);

// 35: get_priority_max()
int sys_get_priority_max(struct syscall_args* args);

// 36: get_priority_min()
int sys_get_priority_min(struct syscall_args* args);

// 37: fsmount(source, target, flags)
int sys_fsmount(struct syscall_args* args);

// 38: copy_file_range(fd_in, fd_out, count)
int sys_copy_file_range(struct syscall_args* args);

// 39: getcwd()
int sys_getcwd(struct syscall_args* args);

// 40: mprotect(addr, len, flags)
int sys_mprotect(struct syscall_args* args);

// 41: mremap(addr, old_len, new_len, flags)
int sys_mremap(struct syscall_args* args);

syscall_function_pointer syscall_dispatch_table[] = {
    [SYSCALL_READ] = sys_read,
    [SYSCALL_WRITE] = sys_write,
    [SYSCALL_FORK] = sys_fork,
    [SYSCALL_OPEN] = sys_open,
    [SYSCALL_CLOSE] = sys_close,
    [SYSCALL_MMAP] = sys_mmap,
    [SYSCALL_SEEK] = sys_seek,
    [SYSCALL_STAT] = sys_stat,
    [SYSCALL_FSTAT] = sys_fstat,
    [SYSCALL_UNLINK] = sys_unlink,
    [SYSCALL_MKDIR] = sys_mkdir,
    [SYSCALL_RMDIR] = sys_rmdir,
    [SYSCALL_TRUNCATE] = sys_truncate,
    [SYSCALL_FTRUNCATE] = sys_ftruncate,
    [SYSCALL_RENAME] = sys_rename,
    [SYSCALL_GETPID] = sys_getpid,
    [SYSCALL_CHDIR] = sys_chdir,
    [SYSCALL_DUP] = sys_dup,
    [SYSCALL_PIPE] = sys_pipe,
    [SYSCALL_CLONE] = sys_clone,
    [SYSCALL_IOCTL] = sys_ioctl,
    [SYSCALL_PRINT] = sys_print,
    [SYSCALL_REBOOT] = sys_reboot,
    [SYSCALL_MUNMAP] = sys_munmap,
    [SYSCALL_CREAT] = sys_creat,
    [SYSCALL_SCHED_YIELD] = sys_sched_yield,
    [SYSCALL_KILL] = sys_kill,
    [SYSCALL_LINK] = sys_link,
    [SYSCALL_GETUID] = sys_getuid,
    [SYSCALL_GETGID] = sys_getgid,
    [SYSCALL_GETEUID] = sys_geteuid,
    [SYSCALL_GETSID] = sys_getsid,
    [SYSCALL_SETUID] = sys_setuid,
    [SYSCALL_SETGID] = sys_setgid,
    [SYSCALL_REGIDT] = sys_regidt,
    [SYSCALL_GET_PRIORITY_MAX] = sys_get_priority_max,
    [SYSCALL_GET_PRIORITY_MIN] = sys_get_priority_min,
    [SYSCALL_FSMOUNT] = sys_fsmount,
    [SYSCALL_COPY_FILE_RANGE] = sys_copy_file_range,
    [SYSCALL_GETCWD] = sys_getcwd,
    [SYSCALL_MPROTECT] = sys_mprotect,
    [SYSCALL_MREMAP] = sys_mremap,
};

extern syscall_table_t syscall_table;

#endif // SYSCALL_H
