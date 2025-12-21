#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../sync/spinlock.h"
#include "../process.h"
#include "../sched.h"
#include "pipe.h"
#include <stdatomic.h>

void pipe_init(pipe_t* pipe) {
    spinlock_init(&pipe->lock);

    refcount_init(&pipe->read_refs);
    refcount_init(&pipe->write_refs);

    pipe->read_bytes = 0;
    pipe->write_bytes = 0;

    pipe->read_fd_open = true;
    pipe->write_fd_open = true;
}

void pipe_close(pipe_t* pipe, int write) {
    spinlock(&pipe->lock);

    bool free_now = false;

    if (write) {
        pipe->write_fd_open = false;
        if (refcount_dec_and_test(&pipe->write_refs))
            free_now = true;
    } else {
        pipe->read_fd_open = false;
        if (refcount_dec_and_test(&pipe->read_refs))
            free_now = true;
    }

    if (!pipe->read_fd_open && !pipe->write_fd_open && atomic_load(&pipe->read_refs) == 0 &&
        atomic_load(&pipe->write_refs) == 0) {
        spinlock_unlock(&pipe->lock, true);
        kfree(pipe, sizeof(pipe_t));
        return;
    }

    spinlock_unlock(&pipe->lock, true);
}

int pipe_write(pipe_t* pipe, char* addr, int len) {
    int written = 0;

    while (written < len) {
        spinlock(&pipe->lock);

        size_t w = pipe->write_bytes % sizeof(pipe->data);
        size_t r = pipe->read_bytes % sizeof(pipe->data);

        // buffer full
        if (((w + 1) % sizeof(pipe->data)) == r) {
            // no readers remain
            if (!pipe->read_fd_open || atomic_load(&pipe->read_refs) == 0 || proc_get_current()->state == TERMINATED) {
                spinlock_unlock(&pipe->lock, true);
                return written ? written : -1;
            }

            spinlock_unlock(&pipe->lock, true);
            sched_yield();
            continue;
        }

        pipe->data[w] = addr[written];
        pipe->write_bytes++;
        written++;

        spinlock_unlock(&pipe->lock, true);
    }

    return written;
}

int pipe_read(pipe_t* pipe, char* addr, int len) {
    int n = 0;

    while (n < len) {
        spinlock(&pipe->lock);

        if (pipe->read_bytes == pipe->write_bytes && atomic_load(&pipe->write_refs) > 0) {
            if (proc_get_current()->state == TERMINATED) {
                spinlock_unlock(&pipe->lock, true);
                return n ? n : -1;
            }

            spinlock_unlock(&pipe->lock, true);
            sched_yield();
            continue;
        }

        if (pipe->read_bytes == pipe->write_bytes && atomic_load(&pipe->write_refs) == 0) {
            spinlock_unlock(&pipe->lock, true);
            return n;
        }

        addr[n] = pipe->data[pipe->read_bytes % sizeof(pipe->data)];
        pipe->read_bytes++;

        n++;

        spinlock_unlock(&pipe->lock, true);
    }

    return n;
}

bool pipe_dup_read(pipe_t* p) {
    return refcount_inc_not_zero(&p->read_refs);
}

bool pipe_dup_write(pipe_t* p) {
    return refcount_inc_not_zero(&p->write_refs);
}
