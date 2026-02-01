/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/

#include "../mem/alloc.h"
#include "../mem/pmm.h"
#include "../mem/paging.h"
#include "../mem/vmm.h"
#include "../mem/utils.h"
#include "../lib/logging.h"
#include "../lib/str.h"
#include "../interrupts/interrupts.h"
#include "../task/sync/spinlock.h"
#include "../drivers/time/floptime.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "sched.h"

uint64_t sched_ticks_counter;

extern process_t* current_process;

static void idle_thread_loop() {
    for (;;) {
    }
}

static void stealer_thread_entry() {}

static thread_t*
sched_internal_init_thread(void (*entry)(void), unsigned int priority, char* name, int user, process_t* process);

// our context switch takes our cpu context struct which
// contains edi, esi, ebx, ebp, eip
// we do not need to save esp because we have it in thread->kernel_stack
extern void context_switch(cpu_ctx_t* old, cpu_ctx_t* new);

// sched_creete_user_thread sets all user threads
// to execute here and this sets up
// the user stack to start executing ip
extern void usermode_entry_routine(uint32_t sp, uint32_t ip);

scheduler_t sched = {
    .kernel_threads = {.head = NULL, .tail = NULL, .count = 0, .name = "kernel_threads", .lock = SPINLOCK_INIT},
    .user_threads = {.head = NULL, .tail = NULL, .count = 0, .name = "user_threads", .lock = SPINLOCK_INIT},
    .ready_queue = {.head = NULL, .tail = NULL, .count = 0, .name = "ready_queue", .lock = SPINLOCK_INIT},
    .sleep_queue = {.head = NULL, .tail = NULL, .count = 0, .name = "sleep_queue", .lock = SPINLOCK_INIT},
    .next_tid = 1,
    .idle_thread = NULL,
    .stealer_thread = NULL,
};

#define USER_STACK_TOP 0xC0000000
#define USER_STACK_SIZE 0x1000

static uintptr_t sched_internal_alloc_user_stack(process_t* process, uint32_t stack_index) {
    uintptr_t user_stack_top = USER_STACK_TOP - (stack_index * USER_STACK_SIZE);
    uintptr_t phys = (uintptr_t) pmm_alloc_page();
    if (!phys) {
        return 0;
    }
    if (vmm_map(process->region, user_stack_top - USER_STACK_SIZE, phys, PAGE_RW | PAGE_USER) < 0) {
        pmm_free_page((void*) phys);
        return 0;
    }

    return user_stack_top;
}

static void sched_internal_setup_thread_stack(thread_t* thread, void (*entry)(void), uintptr_t user_stack_top) {
    uint32_t* kstack = (uint32_t*) ((uintptr_t) thread->kernel_stack + 4096);

    *--kstack = (uint32_t) entry;
    *--kstack = (uint32_t) user_stack_top;

    thread->context.eip = (uint32_t) usermode_entry_routine;
    thread->kernel_stack = kstack;
}

static inline uint32_t sched_internal_fetch_next_stack_index(process_t* process) {
    return process->threads ? process->threads->count : 0;
}

int sched_init_kernel_worker_pool(void);

void sched_init(void) {
    sched.idle_thread = sched_internal_init_thread(idle_thread_loop, 0, "idle", 0, NULL);
    sched.stealer_thread = sched_internal_init_thread(stealer_thread_entry, 0, "reaper", 0, NULL);
    sched_thread_list_add(sched.idle_thread, &sched.ready_queue);

    log("sched: init - ok\n", GREEN);
}

// add thread to the end of a thread queue
// uses the FIFO method
void sched_enqueue(thread_list_t* list, thread_t* thread) {
    if (!list || !thread) {
        return;
    }

    // we must lock when accessesing a thread list
    // this can lead to a race condition if many cores access it at once
    // this isnt a huge concern for now but it does disable interrupts
    // which is important for us here.
    spinlock(&list->lock);

    thread->next = NULL;

    if (!list->head) {
        list->head = thread;
        list->tail = thread;
    } else {
        list->tail->next = thread;
        list->tail = thread;
    }

    // atomic addition for increasing list->count
    atomic_fetch_add_explicit((atomic_uint*) &list->count, 1, memory_order_release);

    spinlock_unlock(&list->lock, true);
}

// remove head of a thread queue
thread_t* sched_dequeue(thread_list_t* list) {
    if (!list) {
        return NULL;
    }

    // we must lock when accessesing a thread list
    spinlock(&list->lock);

    thread_t* thread = list->head;
    if (!thread) {
        spinlock_unlock(&list->lock, true);
        return NULL;
    }

    list->head = thread->next;

    // if the list is empty we set list->tail to NULL which
    // signifies the list is empty.
    if (!list->head) {
        list->tail = NULL;
    }

    atomic_fetch_sub_explicit((atomic_uint*) &list->count, 1, memory_order_release);

    thread->next = NULL;

    spinlock_unlock(&list->lock, true);

    return thread;
}

// remove arbritrary thread_t* "target" from queue.
// slower than dequeue because its (o(n))
// its probably better to use dequeue()
thread_t* sched_remove(thread_list_t* list, thread_t* target) {
    if (!list || !target) {
        return NULL;
    }
    spinlock(&list->lock);
    thread_t* prev = NULL;
    thread_t* curr = list->head;
    while (curr) {
        if (curr == target) {
            if (prev) {
                prev->next = curr->next;
            } else {
                list->head = curr->next;
            }

            if (curr == list->tail) {
                list->tail = prev;
            }

            list->count--;
            curr->next = NULL;

            spinlock_unlock(&list->lock, true);
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }
    spinlock_unlock(&list->lock, true);
    return NULL;
}

#define KERNEL_STACK_PAGES 1
extern vmm_region_t* kernel_region;

static void* sched_internal_init_thread_stack_alloc(thread_t* thread) {
    uintptr_t pa = (uintptr_t) pmm_alloc_pages(0, KERNEL_STACK_PAGES);
    if (!pa) {
        log("sched: pmm_alloc_pages failed\n", RED);
        return NULL;
    }

    uintptr_t va = vmm_alloc(kernel_region, KERNEL_STACK_PAGES, PAGE_PRESENT | PAGE_RW);
    if (va == (uintptr_t) (-1)) {
        pmm_free_pages((void*) pa, 0, KERNEL_STACK_PAGES);
        log("sched: vmm_alloc failed for kernel stack\n", RED);
        return NULL;
    }

    for (size_t i = 0; i < KERNEL_STACK_PAGES; i++) {
        vmm_map(kernel_region, va + i * PAGE_SIZE, pa + i * PAGE_SIZE, PAGE_PRESENT | PAGE_RW);
    }

    thread->kernel_stack = (void*) (va + KERNEL_STACK_PAGES * PAGE_SIZE);
    return thread->kernel_stack;
}

static int sched_init_thread_kernel_or_user_list_insert(thread_t* thread, process_t* process, int user) {
    if (user) {
        thread->user = 1;
        if (process == NULL) {
            // user threads require a process
            log("sched: user thread missing process\n", RED);
            return -1;
        }
        thread->process = process;
    } else {
        thread->user = 0;
        if (process != NULL) {
            log("sched: kernel thread passed non-null process\n", RED);
            return -1;
            // kernel threads cannot have a process
            // that's actually the only thing that makes them kernel threads.
        }
        thread->process = NULL;
        // this must remain null.
    }
    return 0;
}

static thread_t*
sched_internal_init_thread(void (*entry)(void), unsigned priority, char* name, int user, process_t* process) {
    // todo: create slab cache for thread structs
    thread_t* this_thread = kmalloc(sizeof(thread_t));
    if (!this_thread) {
        log("sched: thread struct kmalloc failed\n", RED);
        return NULL;
    }
    flop_memset(this_thread, 0, sizeof(thread_t));

    this_thread->kernel_stack = sched_internal_init_thread_stack_alloc(this_thread);

    this_thread->priority.base = priority;
    this_thread->priority.effective = priority;

    this_thread->context.edi = 0;
    this_thread->context.esi = 0;
    this_thread->context.ebx = 0;
    this_thread->context.ebp = 0;
    this_thread->context.eip = (uint32_t) entry;

    if (sched_init_thread_kernel_or_user_list_insert(this_thread, process, user) < 0) {
        log("sched: thread kernel/user assignment failed\n", RED);
        kfree(this_thread->kernel_stack, 4096);
        kfree(this_thread, sizeof(thread_t));
        return NULL;
    }

    this_thread->id = sched.next_tid++;

    this_thread->name = name;

    this_thread->uptime = 0;
    this_thread->time_since_last_run = 0;
    this_thread->time_slice = priority * 2;

    log_uint("sched: created thread id ", this_thread->id);
    return this_thread;
}

thread_t* sched_create_user_thread(void (*entry)(void), unsigned priority, char* name, process_t* process) {
    if (!process) {
        log("sched: create user thread with null process\n", RED);
        return NULL;
    }

    if (!process->threads) {
        process->threads = kmalloc(sizeof(thread_list_t));
        if (!process->threads) {
            log("sched: process thread list kmalloc failed\n", RED);
            return NULL;
        }
        flop_memset(process->threads, 0, sizeof(thread_list_t));
        spinlock_init(&process->threads->lock);
    }

    thread_t* new_thread = sched_internal_init_thread((void*) usermode_entry_routine, priority, name, 1, process);

    if (!new_thread) {
        log("sched: internal user thread init failed\n", RED);
        return NULL;
    }

    uint32_t stack_index = sched_internal_fetch_next_stack_index(process);
    uintptr_t user_stack_top = sched_internal_alloc_user_stack(process, stack_index);

    if (!user_stack_top) {
        log("sched: user stack allocation failed\n", RED);
        kfree(new_thread->kernel_stack, 4096);
        kfree(new_thread, sizeof(thread_t));
        return NULL;
    }

    sched_internal_setup_thread_stack(new_thread, entry, user_stack_top);

    sched_thread_list_add(new_thread, process->threads);
    sched_thread_list_add(new_thread, &sched.user_threads);

    log("sched: user thread created\n", GREEN);
    return new_thread;
}

extern process_t* current_process;

void sched_thread_list_add(thread_t* thread, thread_list_t* list) {
    if (!thread || !list) {
        return;
    }

    spinlock(&list->lock);

    thread->next = NULL;

    if (!list->head) {
        list->head = thread;
        list->tail = thread;
    } else {
        list->tail->next = thread;
        list->tail = thread;
    }

    list->count++;

    spinlock_unlock(&list->lock, true);
}

thread_t* sched_create_kernel_thread(void (*entry)(void), unsigned priority, char* name) {
    thread_t* new_thread = sched_internal_init_thread((void*) entry, priority, name, 0, NULL);
    log("sched: kernel thread created", GREEN);
    sched_thread_list_add(new_thread, &sched.kernel_threads);
    return new_thread;
}

thread_t* current_thread;

void sched_boost_starved_threads(thread_list_t* list) {
    spinlock(&list->lock);

    for (thread_t* t = list->head; t; t = t->next) {
        t->time_since_last_run++;
        if (t->time_since_last_run > STARVATION_THRESHOLD && t->priority.effective < MAX_PRIORITY) {
            t->priority.effective += BOOST_AMOUNT;
        }
    }

    spinlock_unlock(&list->lock, true);
}

void sched_find_best_thread_iterate(thread_list_t* list,
                                    thread_t** out_best,
                                    thread_t** out_prev_best,
                                    thread_t** out_prev) {
    thread_t* iter = list->head;
    thread_t* prev = NULL;

    while (iter) {
        if (!*out_best || iter->priority.effective > (*out_best)->priority.effective) {
            *out_best = iter;
            *out_prev_best = prev;
        }
        prev = iter;
        iter = iter->next;
    }
}

static thread_t* sched_find_best_thread(thread_list_t* list, thread_t** out_prev) {
    if (!list || !list->head) {
        return NULL;
    }

    thread_t* best = NULL;
    thread_t* best_prev = NULL;

    sched_find_best_thread_iterate(list, &best, &best_prev, NULL);

    if (out_prev) {
        *out_prev = best_prev;
    }

    return best;
}

static void sched_unlink_thread(thread_list_t* list, thread_t* thread, thread_t* prev) {
    if (!list || !thread) {
        return;
    }

    if (prev) {
        prev->next = thread->next;
    } else {
        list->head = thread->next;
    }

    if (list->tail == thread) {
        list->tail = prev;
    }

    if (list->count > 0) {
        list->count--;
    }

    thread->next = NULL;
}

static inline void sched_assign_time_slice(thread_t* t) {
    t->time_slice = t->priority.base ? t->priority.base : 1;
}

static thread_t* sched_select_by_time_slice(thread_list_t* list) {
    if (!list || !list->head) {
        return NULL;
    }

    thread_t* prev = NULL;
    thread_t* best = sched_find_best_thread(list, &prev);
    if (!best) {
        return NULL;
    }
    sched_unlink_thread(list, best, prev);
    sched_assign_time_slice(best);

    return best;
}

static thread_t* sched_select_next(void) {
    sched_boost_starved_threads(&sched.ready_queue);

    spinlock(&sched.ready_queue.lock);
    thread_t* next = sched_select_by_time_slice(&sched.ready_queue);
    spinlock_unlock(&sched.ready_queue.lock, true);

    return next;
}

static thread_t* sched_select_idle_if_needed(thread_t* candidate) {
    if (candidate) {
        return candidate;
    }

    thread_t* idle = sched.idle_thread;
    idle->time_slice = idle->priority.base ? idle->priority.base : 1;
    return idle;
}

static inline bool sched_should_skip(thread_t* next) {
    return next == current_thread;
}

static inline void sched_prepare_thread(thread_t* next) {
    next->time_since_last_run = 0;
}

static void sched_determine_and_switch(thread_t* next) {
    thread_t* prev = current_thread;
    current_thread = next;
    current_process = next->process;

    context_switch(&prev->context, &next->context);
}

void sched_schedule(void) {
    thread_t* next = sched_select_next();
    next = sched_select_idle_if_needed(next);

    if (sched_should_skip(next)) {
        return;
    }

    sched_prepare_thread(next);
    sched_determine_and_switch(next);
}

thread_t* sched_current_thread(void) {
    return current_thread;
}

void sched_thread_exit(void) {
    thread_t* current = sched_current_thread();
    sched_yield();
}

void sched_yield(void) {
    if (!current_thread) {
        return;
    }

    if (current_thread != sched.idle_thread) {
        sched_enqueue(&sched.ready_queue, current_thread);
    }

    sched_schedule();
}

void sched_block(void) {
    thread_t* current = sched_current_thread();
    if (!current) {
        return;
    }

    current->thread_state = THREAD_BLOCKED;

    sched_schedule();
}

void sched_unblock(thread_t* thread) {
    if (!thread) {
        return;
    }

    thread->thread_state = THREAD_READY;
    sched_enqueue(&sched.ready_queue, thread);
}

void sched_thread_sleep(uint32_t ms) {
    thread_t* current = sched_current_thread();
    if (!current || ms == 0) {
        return;
    }

    current->wake_time = sched_ticks_counter + (uint64_t) ms;
    current->thread_state = THREAD_SLEEPING;

    sched_enqueue(&sched.sleep_queue, current);
    sched_yield();
}

static inline bool sched_thread_should_wake(thread_t* t) {
    return t->wake_time <= sched_ticks_counter;
}

static void sched_remove_from_sleep_queue(thread_list_t* sleep_queue, thread_t* thread, thread_t* prev) {
    if (prev) {
        prev->next = thread->next;
    } else {
        sleep_queue->head = thread->next;
    }

    if (sleep_queue->tail == thread) {
        sleep_queue->tail = prev;
    }

    if (sleep_queue->count > 0) {
        sleep_queue->count--;
    }

    thread->next = NULL;
}

static void sched_wake_thread(thread_t* t) {
    t->thread_state = THREAD_READY;
    sched_enqueue(&sched.ready_queue, t);
}

static thread_t* sched_process_sleep_thread(thread_list_t* sleep_queue, thread_t* t, thread_t* prev) {
    thread_t* next = t->next;

    if (sched_thread_should_wake(t)) {
        sched_remove_from_sleep_queue(sleep_queue, t, prev);
        sched_wake_thread(t);
        return prev; // prev stays the same since we removed t
    } else {
        return t; // prev moves forward
    }
}

void sched_tick(void) {
    sched_ticks_counter++;

    spinlock(&sched.sleep_queue.lock);

    thread_t* prev = NULL;
    thread_t* curr = sched.sleep_queue.head;

    while (curr) {
        prev = sched_process_sleep_thread(&sched.sleep_queue, curr, prev);
        curr = (prev) ? prev->next : sched.sleep_queue.head;
    }

    spinlock_unlock(&sched.sleep_queue.lock, true);
}
