#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"
#include "../sched.h"

extern thread_t* current_thread;
#define TURNSTILE_HASH_SIZE 128

typedef struct turnstile {
    void* lock_addr;
    thread_t* owner;
    thread_t* waiters;
    struct turnstile* hash_next;
} turnstile_t;

typedef struct turnstile_chain {
    spinlock_t lock;
    turnstile_t* head;
} turnstile_chain_t;

static turnstile_chain_t turnstile_hash[TURNSTILE_HASH_SIZE];

#define TURNSTILE_HASH_INDEX(lock_addr) (((uintptr_t) lock_addr) >> 3) & (TURNSTILE_HASH_SIZE - 1)

#define TURNSTILE_CHAIN_GET(lock_addr) &turnstile_hash[TURNSTILE_HASH_INDEX(lock_addr)]

static void turnstile_waiters_insert(turnstile_t* ts, thread_t* t) {
    thread_t** cur = &ts->waiters;
    while (*cur && (*cur)->priority.effective >= t->priority.effective) {
        cur = &(*cur)->ts_next;
    }
    t->ts_next = *cur;
    *cur = t;
}

static thread_t* turnstile_waiters_pop(turnstile_t* ts) {
    thread_t* t = ts->waiters;
    if (t) {
        ts->waiters = t->ts_next;
        t->ts_next = NULL;
    }
    return t;
}

static void priority_inheritance_raise(thread_t* owner, int pri) {
    while (owner && owner->priority.effective < pri) {
        owner->priority.effective = pri;
        owner = owner->priority_inheritance_owner;
    }
}

static int priority_inheritance_max_donation(thread_t* owner) {
    int donated = owner->priority.base;

    // check if owner is NULL
    if (!owner) {
        return donated;
    }

    // check if owner has priority inheritance owner
    if (owner->priority_inheritance_owner) {
        donated = owner->priority_inheritance_owner->priority.effective;
    }

    // iterate over turnstile hash table
    for (size_t i = 0; i < TURNSTILE_HASH_SIZE; i++) {
        bool irq = spinlock(&turnstile_hash[i].lock);
        turnstile_t* ts = turnstile_hash[i].head;
        // iterate over turnstile list
        while (ts) {
            // if turnstile owner is owner
            if (ts->owner == owner && ts->owner->priority_inheritance_owner) {
                thread_t* waiter = ts->waiters;
                if (!waiter) {
                    continue;
                }

                // if waiter effective is greater than donated
                // set donated to waiter effective
                if (waiter && waiter->priority.effective > donated) {
                    donated = waiter->priority.effective;
                }
            }
            ts = ts->hash_next;
        }
        spinlock_unlock(&turnstile_hash[i].lock, irq);
    }
    return donated;
}

static void priority_inheritance_unwind(thread_t* owner) {
    while (owner) {
        int max = priority_inheritance_max_donation(owner);
        if (owner->priority.effective == max) {
            return;
        }
        owner->priority.effective = max;
        owner = owner->priority_inheritance_owner;
    }
}

static turnstile_t* turnstile_find_or_create_locked(turnstile_chain_t* chain, void* lock_addr) {
    turnstile_t* ts = chain->head;

    // check if turnstile already exists
    while (ts) {
        // if turnstile already exists, return it
        if (ts->lock_addr == lock_addr) {
            return ts;
        }
        ts = ts->hash_next;
    }

    // allocate and set fields of turnstile
    ts = (turnstile_t*) kmalloc(sizeof(turnstile_t));

    ts->lock_addr = lock_addr;
    ts->owner = NULL;
    ts->waiters = NULL;

    // add new turnstile to the chain
    ts->hash_next = chain->head;
    chain->head = ts;

    return ts;
}

void turnstile_block(void* lock_addr, thread_t* cur, thread_t* owner) {
    // get chain and lock it
    turnstile_chain_t* chain = TURNSTILE_CHAIN_GET(lock_addr);
    bool irq = spinlock(&chain->lock);

    // get turnstile
    turnstile_t* ts = turnstile_find_or_create_locked(chain, lock_addr);

    // add turnstile to the chain
    ts->owner = owner;
    turnstile_waiters_insert(ts, cur);
    cur->priority_inheritance_owner = owner;

    // if owner exists and current thread has higher priority
    // raise priority of owner
    if (owner && cur->priority.effective > owner->priority.effective) {
        priority_inheritance_raise(owner, cur->priority.effective);
    }

    spinlock_unlock(&chain->lock, irq);
    sched_block();
}

#define TURNSTILE_HASH_ITERATE(ts)                                                                                     \
    while (ts && ts->lock_addr != lock_addr) {                                                                         \
        ts = ts->hash_next;                                                                                            \
    }

thread_t* turnstile_unblock(void* lock_addr) {
    // get chain and lock it
    turnstile_chain_t* chain = TURNSTILE_CHAIN_GET(lock_addr);
    bool irq = spinlock(&chain->lock);

    // get turnstile
    turnstile_t* ts = turnstile_find_or_create_locked(chain, lock_addr);

    // iterate through hash chain for the turnstile
    TURNSTILE_HASH_ITERATE(ts)
    // if turnstile not found, unlock chain
    if (!ts) {
        spinlock_unlock(&chain->lock, irq);
        return NULL;
    }

    // get next thread from waiters list
    thread_t* next = turnstile_waiters_pop(ts);

    // if no next thread, unlock chain
    if (!next) {
        ts->owner = NULL;
        spinlock_unlock(&chain->lock, irq);
        return NULL;
    }

    // set owner to turnstile of next thread
    ts->owner = next;
    next->priority_inheritance_owner = NULL;
    spinlock_unlock(&chain->lock, irq);

    priority_inheritance_unwind(current_thread);

    sched_unblock(next);
    return next;
}

bool turnstile_has_waiters(void* lock_addr) {
    turnstile_chain_t* chain = TURNSTILE_CHAIN_GET(lock_addr);
    bool irq = spinlock(&chain->lock);
    turnstile_t* ts = chain->head;
    // iterate through hash chain for the turnstile
    TURNSTILE_HASH_ITERATE(ts)

    bool has = ts && ts->waiters != NULL;
    spinlock_unlock(&chain->lock, irq);
    return has;
}

void turnstile_lock(void* lock_addr, thread_t* owner) {
    turnstile_block(lock_addr, current_thread, owner);
}

void turnstile_unlock(void* lock_addr) {
    turnstile_unblock(lock_addr);
}
