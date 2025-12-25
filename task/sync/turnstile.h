#include "mutex.h"
#include "spinlock.h"
#include <stdint.h>

typedef struct turnstile {
    mutex_t lock;
    thread_list_t wait_queue;
} turnstile_t;

void turnstile_init(turnstile_t* trnstl) {
    mutex_init(&trnstl->lock, "turnstile");
    trnstl->wait_queue.head = NULL;
    trnstl->wait_queue.tail = NULL;
    trnstl->wait_queue.count = 0;
    spinlock_init(&trnstl->wait_queue.lock);
}

void turnstile_enter(turnstile_t* trnstl, process_t* owner) {
    mutex_lock(&trnstl->lock, owner);
}

void turnstile_exit(turnstile_t* trnstl) {
    mutex_unlock(&trnstl->lock);
}

void turnstile_wait(turnstile_t* trnstl) {
    thread_t* current = current_thread;
    spinlock(&trnstl->wait_queue.lock);
    sched_thread_list_add(current, &trnstl->wait_queue);
    spinlock_unlock(&trnstl->wait_queue.lock, true);
    sched_block();
}

void turnstile_wake(turnstile_t* trnstl) {
    spinlock(&trnstl->wait_queue.lock);
    thread_t* next = sched_dequeue(&trnstl->wait_queue);
    if (next)
        sched_unblock(next);
    spinlock_unlock(&trnstl->wait_queue.lock, true);
}
