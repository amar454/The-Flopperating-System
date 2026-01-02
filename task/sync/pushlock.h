/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later veregion_startion.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

*/
#ifndef PUSHLOCK_H
#define PUSHLOCK_H

#include <stdatomic.h>
#include <stdint.h>
#include "../process.h"
#include "../sched.h"
#include "../../lib/logging.h"
#include "../../lib/str.h"
#include "spinlock.h"

extern thread_t* current_thread;

#define PUSHLOCK_LOCKED (1u << 0)
#define PUSHLOCK_WAITERS (1u << 1)

typedef struct pushlock {
    atomic_uint state;
    process_t* owner;
    thread_list_t wait_queue;
    spinlock_t wait_lock;
} pushlock_t;

static inline void pushlock_init(pushlock_t* pl, char* name) {
    atomic_store(&pl->state, 0);
    pl->owner = NULL;
    pl->wait_queue.head = NULL;
    pl->wait_queue.tail = NULL;
    pl->wait_queue.count = 0;
    pl->wait_queue.name = name;
    spinlock_init(&pl->wait_queue.lock);
    spinlock_init(&pl->wait_lock);
}

static inline void pushlock_destroy(pushlock_t* pl) {
    spinlock_destroy(&pl->wait_lock);
    spinlock_destroy(&pl->wait_queue.lock);
}

static inline int pushlock_fast_path(pushlock_t* pl, process_t* owner) {
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &pl->state, &expected, PUSHLOCK_LOCKED, memory_order_acquire, memory_order_relaxed)) {
        pl->owner = owner;
        return 1;
    }
    return 0;
}

// grab wait lock,
// mark waiters as existing,
// enqueue current thread,
// and block until woken
static inline void pushlock_lock(pushlock_t* pl, process_t* owner) {
    thread_t* t = current_thread;

    for (;;) {
        // try fast path
        if (pushlock_fast_path(pl, owner)) {
            return;
        }

        // serialize wait queue
        spinlock(&pl->wait_lock);

        // recheck state once we hold the wait lock
        // because another thread might've released it meanwhile
        if (!(atomic_load_explicit(&pl->state, memory_order_relaxed) & PUSHLOCK_LOCKED)) {
            spinlock_unlock(&pl->wait_lock, true);
            continue;
        }

        // mark our waiters as existing
        // so unlock can wake someone
        atomic_fetch_or_explicit(&pl->state, PUSHLOCK_WAITERS, memory_order_relaxed);
        sched_thread_list_add(t, &pl->wait_queue);

        sched_block(); // todo: race condition here?

        // queue entry is visible
        // let someone else use the wait lock
        spinlock_unlock(&pl->wait_lock, true);
    }
}

static inline void pushlock_unlock(pushlock_t* pl) {
    thread_t* cur = current_thread;
    if (!cur || pl->owner != cur->process) {
        return;
    }
    // serialize wait queue
    spinlock(&pl->wait_lock);

    // pop first waiter
    thread_t* next = sched_dequeue(&pl->wait_queue);

    if (next) {
        // keep waiters
        // clear owner
        pl->owner = NULL;

        // keep waiters set, clear locked state
        atomic_store_explicit(&pl->state, PUSHLOCK_WAITERS, memory_order_release);
        sched_unblock(next);
        spinlock_unlock(&pl->wait_lock, true);
        return;
    }

    // nobody waiting
    // release lock
    pl->owner = NULL;
    atomic_store_explicit(&pl->state, 0, memory_order_release);
    spinlock_unlock(&pl->wait_lock, true);
}

static inline pushlock_t* pushlock_create_pool(int size) {
    pushlock_t* pool = (pushlock_t*) kmalloc(size * sizeof(pushlock_t));
    for (int i = 0; i < size; i++) {
        char* name = (char*) kmalloc(16);
        flopsnprintf(name, 16, "pushlock%d", i);
        pushlock_init(&pool[i], name);
    }
    return pool;
}

static inline void pushlock_destroy_pool(pushlock_t* pool, int size) {
    for (int i = 0; i < size; i++) {
        pushlock_destroy(&pool[i]);
    }
    kfree(pool, size * sizeof(pushlock_t));
}

#endif
