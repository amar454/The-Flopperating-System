#pragma once
#include "spinlock.h"
#include <stdint.h>
#include <stdatomic.h>
#include "../process.h"
#include "../sched.h"
#include "../../lib/assert.h"
#include "../../lib/logging.h"
extern thread_t* current_thread;

typedef enum {
    MUTEX_UNLOCKED,
    MUTEX_LOCKED
} mutex_state_t;

typedef struct mutex {
    atomic_int state;
    thread_t* owner;
    thread_list_t wait_queue;
    spinlock_t wait_lock;
} mutex_t;

static inline void mutex_init(mutex_t* mutex, char* name) {
    atomic_store(&mutex->state, MUTEX_UNLOCKED);
    mutex->owner = NULL;

    mutex->wait_queue.head = NULL;
    mutex->wait_queue.tail = NULL;
    mutex->wait_queue.count = 0;
    mutex->wait_queue.name = name;
    spinlock_init(&mutex->wait_queue.lock);

    spinlock_init(&mutex->wait_lock);
}

static inline void mutex_lock(mutex_t* mutex, thread_t* owner) {
    thread_t* current = current_thread;

    for (;;) {
        int expected = MUTEX_UNLOCKED;

        // wait for mutex to be unlocked
        if (atomic_compare_exchange_strong_explicit(
                &mutex->state, &expected, MUTEX_LOCKED, memory_order_acquire, memory_order_relaxed)) {
            mutex->owner = owner;
            return;
        }

        spinlock(&mutex->wait_lock);

        // if mutex is unlocked, unlock wait_lock and try again
        if (atomic_load_explicit(&mutex->state, memory_order_relaxed) == MUTEX_UNLOCKED) {
            spinlock_unlock(&mutex->wait_lock, true);
            continue;
        }

        // add to mutex wait queue
        sched_thread_list_add(current, &mutex->wait_queue);

        spinlock_unlock(&mutex->wait_lock, true);

        // sets thread state to blocked
        sched_block();
    }
}

static inline void mutex_unlock(mutex_t* mutex) {
    thread_t* current = current_thread;

    // if any of this is true, we should not be here
    if (!current || mutex->owner != current) {
        log("mutex: what are you doing bruh", RED);
        return;
    }

    spinlock(&mutex->wait_lock);

    thread_t* next = sched_dequeue(&mutex->wait_queue);

    if (next) {
        // if there is a waiting thread, transfer ownership keep it in locked state
        mutex->owner = next;

        // wake the sleeping thread
        sched_unblock(next);
        spinlock_unlock(&mutex->wait_lock, true);
        return;
    }

    // no threads waiting
    // release mutex, set state to unlocked
    mutex->owner = NULL;
    atomic_store_explicit(&mutex->state, 0, memory_order_release);

    spinlock_unlock(&mutex->wait_lock, true);
}
