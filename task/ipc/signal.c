#include "signal.h"
#include "../sched.h"
#include "../process.h"
#include "../sync/spinlock.h"

#define TERMINATE_CASE                                                                                                 \
    case SIGACT_TERMINATE:                                                                                             \
    case SIGACT_CORE

static signal_action_t default_actions[SIGMAX] = {
    [SIGINT] = SIGACT_TERMINATE,
    [SIGKILL] = SIGACT_TERMINATE,
    [SIGSEGV] = SIGACT_CORE,
    [SIGTERM] = SIGACT_TERMINATE,
    [SIGUSR1] = SIGACT_IGNORE,
    [SIGUSR2] = SIGACT_IGNORE,
    [SIGSTOP] = SIGACT_STOP,
    [SIGCONT] = SIGACT_CONTINUE,
};

void signal_init_process(process_t* process) {
    if (!process) {
        return;
    }

    spinlock_init(&process->sig_lock);
    spinlock(&process->sig_lock);

    // no pending signals to begin with
    process->sig_pending = 0;
    // no masked signals to begin with
    process->sig_mask = 0;

    // clear all the signal handlers
    for (int i = 0; i < SIGMAX; i++) {
        process->sig_handlers[i] = NULL;
    }

    spinlock_unlock(&process->sig_lock, true);
}

int signal_set_handler(process_t* process, int sig, signal_handler_t handler) {
    if (!process || sig <= 0 || sig >= SIGMAX) {
        return -1;
    }

    // cannot override kill and stop
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -1;
    }
    spinlock(&process->sig_lock);
    process->sig_handlers[sig] = handler;
    spinlock_unlock(&process->sig_lock, true);
    return 0;
}

int signal_send(process_t* target, int sig) {
    if (!target || sig <= 0 || sig >= SIGMAX) {
        return -1;
    }
    spinlock(&target->sig_lock);

    // mark signal as pending
    target->sig_pending |= (1u << sig);
    spinlock_unlock(&target->sig_lock, true);
    return 0;
}

static void signal_default_action(process_t* process, int sig) {
    switch (default_actions[sig]) {
        case SIGACT_IGNORE:
            // nothing to do
            return;
        TERMINATE_CASE:
            // terminate the process
            proc_exit(process, 128 + sig);
            return;
        case SIGACT_STOP:
            // stop the process
            proc_stop(process);
            return;
        case SIGACT_CONTINUE:
            // continue stopped process
            proc_continue(process);
            return;
        case SIGACT_HANDLER:
            // nothing to do
            // will be called elsewhere
            return;
    }
}

void signal_dispatch(process_t* process) {
    if (!process) {
        return;
    }
    spinlock(&process->sig_lock);
    uint32_t pending = process->sig_pending;

    // if no pending signals, unlock and return
    if (!pending) {
        spinlock_unlock(&process->sig_lock, true);
        return;
    }

    // iterate through signals
    for (int sig = 1; sig < SIGMAX; sig++) {
        uint32_t bit = (1u << sig);

        // skip signals that are not pending
        if (!(pending & bit)) {
            continue;
        }

        // skip signals that are masked
        // except for SIGKILL and SIGSTOP
        if ((process->sig_mask & bit) && sig != SIGKILL && sig != SIGSTOP) {
            continue;
        }

        // clear pending bit
        process->sig_pending &= ~bit;

        // get handler and action
        signal_handler_t handler = process->sig_handlers[sig];
        signal_action_t action = default_actions[sig];

        // unlock while executing handler
        // prevents deadlocks?
        spinlock_unlock(&process->sig_lock, true);

        // execute handler if available
        if (handler && sig != SIGKILL && sig != SIGSTOP) {
            handler(sig);
        }
        // call default action if handler is not available
        else if (action != SIGACT_HANDLER) {
            signal_default_action(process, sig);
        }
        // reacquire lock before continuing
        spinlock(&process->sig_lock);
    }

    // after all signals are done we can unlock
    spinlock_unlock(&process->sig_lock, true);
}
