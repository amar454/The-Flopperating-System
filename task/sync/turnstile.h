#include "mutex.h"
#include "spinlock.h"
#include "../sched.h"
#include <stdint.h>

typedef struct turnstile {
    mutex_t lock;
    thread_list_t wait_queue;
    spinlock_t wait_lock;
} turnstile_t;

void turnstile_init(turnstile_t* t) {
    mutex_init(&t->lock, "turnstile");
    t->wait_queue.head = NULL;
    t->wait_queue.tail = NULL;
    t->wait_queue.count = 0;
    spinlock_init(&t->wait_queue.lock);
    spinlock_init(&t->wait_lock);
}

void turnstile_enter(turnstile_t* t, process_t* owner) {
    mutex_lock(&t->lock, owner);
}

void turnstile_exit(turnstile_t* t) {
    mutex_unlock(&t->lock);
}

void turnstile_wait(turnstile_t* t) {
    thread_t* cur = current_thread;
    spinlock(&t->wait_lock);
    thread_t* prev = NULL;
    thread_t* iter = t->wait_queue.head;

    while (iter && iter->priority.effective >= cur->priority.effective) {
        prev = iter;
        iter = iter->next;
    }

    if (!prev) {
        cur->next = t->wait_queue.head;
        t->wait_queue.head = cur;
        if (!t->wait_queue.tail)
            t->wait_queue.tail = cur;
    } else {
        cur->next = prev->next;
        prev->next = cur;
        if (!cur->next)
            t->wait_queue.tail = cur;
    }

    t->wait_queue.count++;
    spinlock_unlock(&t->wait_lock, true);
    sched_block();
}

void turnstile_wake(turnstile_t* t) {
    spinlock(&t->wait_lock);
    thread_t* highest = t->wait_queue.head;
    thread_t* prev = NULL;
    thread_t* iter = t->wait_queue.head;
    thread_t* prev_high = NULL;

    while (iter) {
        if (iter->priority.effective > highest->priority.effective) {
            highest = iter;
            prev_high = prev;
        }
        prev = iter;
        iter = iter->next;
    }

    if (highest) {
        if (prev_high)
            prev_high->next = highest->next;
        else
            t->wait_queue.head = highest->next;

        if (highest == t->wait_queue.tail)
            t->wait_queue.tail = prev_high;

        highest->next = NULL;
        t->wait_queue.count--;
        sched_unblock(highest);
    }

    spinlock_unlock(&t->wait_lock, true);
}

void turnstile_wake_all(turnstile_t* t) {
    spinlock(&t->wait_lock);
    while (t->wait_queue.head) {
        turnstile_wake(t);
    }
    spinlock_unlock(&t->wait_lock, true);
}

int turnstile_empty(turnstile_t* t) {
    spinlock(&t->wait_lock);
    int empty = t->wait_queue.count == 0;
    spinlock_unlock(&t->wait_lock, true);
    return empty;
}
