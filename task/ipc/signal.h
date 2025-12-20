#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SIG_NONE 0
#define SIGINT 2
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15
#define SIGUSR1 16
#define SIGUSR2 17
#define SIGSTOP 19
#define SIGCONT 18
#define SIGMAX 32

typedef struct process process_t;

typedef void (*signal_handler_t)(int);

typedef enum {
    SIGACT_IGNORE,
    SIGACT_TERMINATE,
    SIGACT_CORE,
    SIGACT_STOP,
    SIGACT_CONTINUE,
    SIGACT_HANDLER,
} signal_action_t;

typedef struct signal_entry {
    signal_handler_t handler;
    signal_action_t action;
} signal_entry_t;

typedef struct signal_state {
    uint32_t pending;
    signal_entry_t table[SIGMAX];
} signal_state_t;

void signal_init_process(process_t* process);
int signal_set_handler(process_t* process, int sig, signal_handler_t handler);
int signal_send(process_t* target, int sig);
void signal_dispatch(process_t* process);

#endif
