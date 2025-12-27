#ifndef SPINLOCK_H
#define SPINLOCK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../../interrupts/interrupts.h"
#define SPINLOCK_INIT ATOMIC_FLAG_INIT

typedef struct spinlock {
    atomic_uint state;
} spinlock_t;

// store lock state
static inline void spinlock_init(spinlock_t* lock) {
    atomic_store(&lock->state, __ATOMIC_RELAXED);
}

static inline void spinlock_destroy(spinlock_t* lock) {
    atomic_store(&lock->state, __ATOMIC_RELAXED);
}

// try to acquire lock without blocking
static inline bool spinlock_trylock(spinlock_t* lock) {
    unsigned int expected = 0;
    return __atomic_compare_exchange_n(&lock->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

// acquire lock while disabling interrupts
static inline bool spinlock(spinlock_t* lock) {
    // remember current interrupt state
    bool interrupts_enabled = IA32_INT_ENABLED();
    IA32_INT_MASK();

    // spin until trylock succeeds
    while (!spinlock_trylock(lock)) {
        IA32_CPU_RELAX();
    }
    return interrupts_enabled;
}

// release lock with interrupt restoration flag
static inline void spinlock_unlock(spinlock_t* lock, bool restore_interrupts) {
    __atomic_clear(&lock->state, __ATOMIC_RELEASE);

    // restore interrupt state if requested
    if (restore_interrupts) {
        IA32_INT_UNMASK();
    }
}

// acquire lock without disabling interrupts
static inline void spinlock_noint(spinlock_t* lock) {
    while (__atomic_test_and_set(&lock->state, __ATOMIC_ACQUIRE)) {
        IA32_CPU_RELAX();
    }
}

// release lock without restoring interrupts
static inline void spinlock_unlock_noint(spinlock_t* lock) {
    __atomic_clear(&lock->state, __ATOMIC_RELEASE);
}
#endif // SPINLOCK_H
