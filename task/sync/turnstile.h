/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of FloppaOS.

FloppaOS is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later veregion_startion.

FloppaOS is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with FloppaOS. If not, see <https://www.gnu.org/licenses/>.

*/
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "../sched.h"

extern thread_t* current_thread;

#define TURNSTILE_HASH_SIZE 128

// ridiculously long hash macro
#define TURNSTILE_HASH_INDEX(lock_addr) ((((uintptr_t) (lock_addr)) >> 3) & (TURNSTILE_HASH_SIZE - 1))

typedef struct turnstile {
    void* lock_addr;
    thread_t* owner;
    thread_t* waiters;
} turnstile_t;

typedef struct turnstile_bucket {
    spinlock_t lock;
    bool initialized;
    turnstile_t ts;
} turnstile_bucket_t;

static turnstile_bucket_t turnstile_table[TURNSTILE_HASH_SIZE];

static void turnstile_waiters_insert(turnstile_t* ts, thread_t* t) {
    thread_t** cur = &ts->waiters;

    // insert in descending effective-priority order
    while (*cur && (*cur)->priority.effective >= t->priority.effective) {
        cur = &(*cur)->ts_next;
    }

    // splice into list
    t->ts_next = *cur;
    *cur = t;
}

static thread_t* turnstile_waiters_pop(turnstile_t* ts) {
    // get the highest-priority waiter
    thread_t* t = ts->waiters;

    if (t) {
        // unlink from list head
        ts->waiters = t->ts_next;
        t->ts_next = NULL;
    }

    return t;
}

static void priority_inheritance_raise(thread_t* owner, int pri) {
    // walk inheritance chain and raise priority
    while (owner && owner->priority.effective < pri) {
        owner->priority.effective = pri;
        owner = owner->priority_inheritance_owner;
    }
}

static turnstile_t* turnstile_get_locked(void* lock_addr, bool* irq) {
    // get bucket index
    size_t idx = TURNSTILE_HASH_INDEX(lock_addr);
    turnstile_bucket_t* b = &turnstile_table[idx];

    // lock bucket
    *irq = spinlock(&b->lock);

    // lazily initialize bucket turnstile
    if (!b->initialized) {
        b->initialized = true;
        b->ts.lock_addr = lock_addr;
        b->ts.owner = NULL;
        b->ts.waiters = NULL;
    }

    return &b->ts;
}

static void turnstile_unlock_bucket(void* lock_addr, bool irq) {
    // get bucket index
    size_t idx = TURNSTILE_HASH_INDEX(lock_addr);

    // unlock fetched bucket
    spinlock_unlock(&turnstile_table[idx].lock, irq);
}

static int priority_inheritance_max_donation(thread_t* owner) {
    // nothing to do if no owner
    if (!owner) {
        return 0;
    }

    int max = owner->priority.base;

    // check the lock this owner is blocked on
    void* lock_addr = owner->blocked_lock;

    if (!lock_addr) {
        return max;
    }

    // get waiters of that lock's turnstile
    bool irq;
    turnstile_t* ts = turnstile_get_locked(lock_addr, &irq);
    thread_t* w = ts->waiters;

    // get highest donation candidate
    if (w && w->priority.effective > max) {
        max = w->priority.effective;
    }

    turnstile_unlock_bucket(lock_addr, irq);
    return max;
}

static void priority_inheritance_unwind(thread_t* owner) {
    // fetch priorities along the chain
    while (owner) {
        int max = priority_inheritance_max_donation(owner);

        // stop if already correct
        if (owner->priority.effective == max) {
            return;
        }

        // lower or raise to fetched value
        owner->priority.effective = max;
        owner = owner->priority_inheritance_owner;
    }
}

void turnstile_block(void* lock_addr, thread_t* cur, thread_t* owner) {
    bool irq;

    // fetch and lock turnstile bucket
    turnstile_t* ts = turnstile_get_locked(lock_addr, &irq);

    ts->owner = owner;

    // queue current thread to waiters
    turnstile_waiters_insert(ts, cur);

    cur->priority_inheritance_owner = owner;
    cur->blocked_lock = lock_addr;

    // donate priority if needed
    if (owner && cur->priority.effective > owner->priority.effective) {
        priority_inheritance_raise(owner, cur->priority.effective);
    }

    turnstile_unlock_bucket(lock_addr, irq);
    sched_block();
}

thread_t* turnstile_unblock(void* lock_addr) {
    bool irq;

    // fetch and lock turnstile bucket
    turnstile_t* ts = turnstile_get_locked(lock_addr, &irq);

    // get next waiter
    thread_t* next = turnstile_waiters_pop(ts);

    // if no waiters null owner
    if (!next) {
        ts->owner = NULL;
        turnstile_unlock_bucket(lock_addr, irq);
        return NULL;
    }

    // assign ownership to next thread
    ts->owner = next;
    next->priority_inheritance_owner = NULL;
    next->blocked_lock = NULL;

    // release bucket before unwinding inheritance
    turnstile_unlock_bucket(lock_addr, irq);

    // fetch donated priorities
    priority_inheritance_unwind(current_thread);

    // wake next
    sched_unblock(next);
    return next;
}

bool turnstile_has_waiters(void* lock_addr) {
    bool irq;

    // check bucket waiters list
    turnstile_t* ts = turnstile_get_locked(lock_addr, &irq);

    // check if waiters list is empty
    bool has = ts->waiters != NULL;

    turnstile_unlock_bucket(lock_addr, irq);

    // return true if waiters list is not empty
    return has;
}

// kernel api wrappers
void turnstile_lock(void* lock_addr, thread_t* owner) {
    turnstile_block(lock_addr, current_thread, owner);
}

void turnstile_unlock(void* lock_addr) {
    turnstile_unblock(lock_addr);
}
