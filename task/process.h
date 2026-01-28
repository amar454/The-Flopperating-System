#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "sync/spinlock.h"
#include "../mem/alloc.h"
#include "../mem/pmm.h"
#include "../mem/vmm.h"
#include "../fs/vfs/vfs.h"
#include "sched.h"
#include "ipc/signal.h"

typedef struct process process_t;
typedef signed int pid_t;
typedef int uid_t;
typedef struct thread_list thread_list_t;
extern process_t* current_process;
#define MAX_PROCESSES 256

typedef enum process_state {
    RUNNING,
    RUNNABLE,
    SLEEPING,
    STOPPED,
    EMBRYO,
    ZOMBIE,
    TERMINATED
} process_state_t;

typedef struct proc_table {
    spinlock_t proc_table_lock;
    process_t* processes;
} proc_table_t;

typedef struct proc_info {
    pid_t next_pid;
    proc_table_t* table;
    int process_count;
} proc_info_t;

#define MAX_PROC_FDS 128

// data structure representing a process
// a process has its own address space
// and its own list of threads
// each process has a parent
typedef struct process {
    // each process has its own linked list of threads.
    thread_list_t* threads;

    // address space
    vmm_region_t* region;

    // memory usage in bytes
    // for now we can just increment this when we allocate
    // TODO: there's probably a better way of tracking this.
    uint32_t mem_usage;

    // we can add a pointer to the procfs node of the process
    // but we can easily get the path by looking up /process/[PID]
    // the cwd is still necessary though (sysv)
    struct vfs_node* cwd;

    // file descriptor table
    struct vfs_file_descriptor fds[MAX_PROC_FDS];

    // all processes have a parent
    struct process* parent;

    // processes owned by this process
    struct process* children;

    // processes with the same parent as this process
    struct process* siblings;

    // process id
    pid_t pid;

    // session id
    pid_t sid;

    // process group id
    pid_t pgid;

    // group id and effective group id
    pid_t rgid;
    pid_t gid;

    // user id and effective user id
    uid_t ruid;
    uid_t uid;

    // process state
    // NOTE: this is different from thread state
    process_state_t state;

    // NOTE: allocate for name.
    // TODO: this is kinda shit.
    char* name;

    // process pipes
    pipe_t* pipe;

    int exit_status;

    signal_handler_t sig_handlers[SIGMAX];

    uint32_t sig_mask;

    uint32_t sig_pending;

    spinlock_t sig_lock;
} process_t;

process_t* proc_get_current();
int proc_create_init_process();
pid_t proc_getpid(process_t* process);
int proc_kill(process_t* process);
int proc_stop(process_t* process);
int proc_continue(process_t* process);
pid_t proc_fork(process_t* parent);
int proc_init();
int proc_exit(process_t* process, int status);
pid_t proc_dup(pid_t pid);
process_t* proc_get_process_by_pid(pid_t pid);
int proc_exit_all_threads(process_t* process);
#endif
