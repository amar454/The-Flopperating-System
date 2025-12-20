#include "signal.h"
#include "../sched.h"
#include "../process.h"
#include "../sync/spinlock.h"

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

    process->sig_pending = 0;
    process->sig_mask = 0;

    for (int i = 0; i < SIGMAX; i++) {
        process->sig_handlers[i] = NULL;
    }

    spinlock_unlock(&process->sig_lock, true);
}

int signal_set_handler(process_t* process, int sig, signal_handler_t handler) {
    if (!process || sig <= 0 || sig >= SIGMAX) {
        return -1;
    }

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

    target->sig_pending |= (1u << sig);

    spinlock_unlock(&target->sig_lock, true);
    return 0;
}

static void signal_default_action(process_t* process, int sig) {
    signal_action_t act = default_actions[sig];

    switch (act) {
        case SIGACT_IGNORE: {
            return;
        }

        case SIGACT_TERMINATE: {
            proc_exit(process, 128 + sig);
            return;
        }

        case SIGACT_CORE: {
            proc_exit(process, 128 + sig);
            return;
        }

        case SIGACT_STOP: {
            proc_stop(process);
            return;
        }

        case SIGACT_CONTINUE: {
            proc_continue(process);
            return;
        }

        case SIGACT_HANDLER: {
            return;
        }
    }
}

void signal_dispatch(process_t* process) {
    if (!process) {
        return;
    }

    spinlock(&process->sig_lock);

    uint32_t pending = process->sig_pending;
    if (!pending) {
        spinlock_unlock(&process->sig_lock, true);
        return;
    }

    for (int sig = 1; sig < SIGMAX; sig++) {
        uint32_t bit = (1u << sig);

        if (!(pending & bit)) {
            continue;
        }

        if ((process->sig_mask & bit) && sig != SIGKILL && sig != SIGSTOP) {
            continue;
        }

        process->sig_pending &= ~bit;

        signal_handler_t handler = process->sig_handlers[sig];
        signal_action_t action = default_actions[sig];

        spinlock_unlock(&process->sig_lock, true);

        if (handler && sig != SIGKILL && sig != SIGSTOP) {
            handler(sig);
        } else if (action == SIGACT_HANDLER) {
        } else {
            signal_default_action(process, sig);
        }

        spinlock(&process->sig_lock);
    }

    spinlock_unlock(&process->sig_lock, true);
}
