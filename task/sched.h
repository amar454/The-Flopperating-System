#ifndef SCHED_H
#define SCHED_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "sync/spinlock.h"
#include "../mem/alloc.h"
#include "../mem/pmm.h"
#include "../mem/vmm.h"
#include "ipc/signal.h"
#include "../fs/vfs/vfs.h"
#include "process.h"
typedef struct process process_t;

// state of the cpu upon a context switch
// this will be used as the parameters old and new
// within our context switch function
typedef struct cpu_ctx {
    uint32_t edi;
    // we dont need the stack pointer here.
    // we have it at thread->kernel_stack
    uint32_t esi;
    uint32_t ebx;
    uint32_t ebp;
    uint32_t eip;
} cpu_ctx_t;

typedef enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_EXITED,
    THREAD_DEAD,
    THREAD_SUSPENDED
} thread_state_t;

typedef struct thread thread_t;

typedef struct thread_priority {
    unsigned base;
    unsigned effective;
} thread_priority_t;

#define STARVATION_THRESHOLD 1000
#define BOOST_AMOUNT 5
#define MAX_PRIORITY 255

typedef struct thread_list {
    thread_t* head;
    thread_t* tail;
    uint32_t count;
    char* name;
    spinlock_t lock;
} thread_list_t;

typedef struct reaper_descriptor {
    thread_list_t dead_threads;
    spinlock_t lock;
    int running;
    signal_t wake_signal;
    thread_t* reaper_thread;
} reaper_descriptor_t;

// data structure representing a thread
// a thread is a user thread when it has a process
// upon thread creation, if the thread is a kernel thread
// it will have no process and will inherit the kernel address space
// if the thread is a user thread, it will have a process
// and will inherit the process address space
typedef struct thread {
    thread_t* next;
    thread_t* previous;
    void* kernel_stack;
    int user;
    cpu_ctx_t context;

    // priority is the base priority assigned when the thread is created
    // if the thread is starved a priority boost will be added to the effective priority
    // if the effective priority is higher than the base priority
    // the scheduler will treat the effective priority of the thread to be the priority
    // within sched_schedule()
    thread_priority_t priority;

    thread_state_t thread_state;
    // if a thread is a kernel thread, it has no process.
    // the thread will inherit the kernel address space.
    process_t* process;

    // identification
    uint32_t id;

    // NOTE: allocate for name.
    char* name;

    // time info
    // time since last run is important
    // because we can tell if its being starved
    // if its significantly high, we can boost its priority
    uint32_t uptime;
    uint32_t time_since_last_run;
    uint32_t time_slice;

    uint64_t wake_time;
} thread_t;

typedef struct scheduler {
    thread_list_t* ready_queue;
    thread_list_t* sleep_queue;
    thread_list_t* kernel_threads;
    thread_list_t* user_threads;
    spinlock_t ready_queue_lock;
    uint32_t next_tid;
    thread_t* idle_thread;
    thread_t* reaper_thread;
    thread_t* stealer_thread;
} scheduler_t;

extern scheduler_t sched;

thread_t* sched_create_kernel_thread(void (*entry)(void), unsigned priority, char* name);

// use this for everything but the ready queue.
void sched_thread_list_add(thread_t* thread, thread_list_t* list);
thread_t* sched_create_user_thread(void (*entry)(void), unsigned priority, char* name, process_t* process);
void sched_init(void);
void sched_enqueue(thread_list_t* list, thread_t* thread);
thread_t* sched_dequeue(thread_list_t* list);
thread_t* sched_remove(thread_list_t* list, thread_t* target);
void sched_schedule(void);
void sched_yield(void);

extern void sched_tick(void);
#endif // SCHED_H
