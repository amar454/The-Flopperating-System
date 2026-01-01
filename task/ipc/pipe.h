#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stddef.h>
#include "../sync/spinlock.h"
#include "../../lib/refcount.h"
#include "../../mem/alloc.h"

typedef struct pipe {
    spinlock_t lock;
    uint32_t read_bytes;
    uint32_t write_bytes;
    refcount_t read_refs;
    refcount_t write_refs;
    bool read_fd_open;
    bool write_fd_open;
    char data[512];
} pipe_t;

void pipe_init(pipe_t* pipe);
void pipe_close(pipe_t* pipe, int write);
int pipe_write(pipe_t* pipe, char* addr, int len);
int pipe_read(pipe_t* pipe, char* addr, int len);
bool pipe_dup_read(pipe_t* p);
bool pipe_dup_write(pipe_t* p);
#endif // PIPE_H
