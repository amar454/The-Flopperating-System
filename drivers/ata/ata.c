/*

Copyright 2024-2026 Amar Djulovic <aaamargml@gmail.com>

This file is part of The Flopperating System.

The Flopperating System is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either veregion_startion 3 of the License, or (at your option) any later version.

The Flopperating System is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with The Flopperating System. If not, see <https://www.gnu.org/licenses/>.

*/
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../io/io.h"
#include "../../lib/logging.h"
#include "../../task/sched.h"
#include "ata.h"
#include "../../interrupts/interrupts.h"

extern thread_t* current_thread;

ata_queue_t ata_queue;
static spinlock_t ata_lock = SPINLOCK_INIT;

static void ata_bs_wait(void) {
    // wait for bsy to clear and rdy to be set
    while ((inb(ATA_PORT_STATUS) & ATA_BSY) || !(inb(ATA_PORT_STATUS) & ATA_RDY)) {
        IA32_CPU_RELAX();
    }
}

static void ata_drq_wait(void) {
    while (!(inb(ATA_PORT_STATUS) & ATA_DRQ)) {
        IA32_CPU_RELAX();
    }
}

static int ata_op_validate(uint8_t sectors, uint8_t* buffer) {
    if (!buffer || sectors == 0) {
        log_uint("invalid sector count or buffer is garbage, sec num: ", sectors);
        return -1;
    }

    if (sectors > (uint8_t) MAX_SECTORS) {
        log_uint("ata: too many sectors: ", sectors);
        return -1;
    }

    return 0; // request is valid
}

void ata_init_drive(uint8_t drive_num) {
    // send init command
    ATA_DRIVE_INIT_PREPARE(drive_num);

    // wait for drive to wake up
    // sleepy boi
    while ((inb(ATA_PORT_STATUS) & ATA_BSY) == 0) {
        inb(ATA_PORT_STATUS);
    }
}

void ata_queue_init(void) {
    ata_queue.head = NULL;
    ata_queue.tail = NULL;
    ata_queue.length = 0;
    spinlock_init(&ata_queue.lock);
}

// read sectors into buffer
void ata_read(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer, bool queued) {
    if (ata_op_validate(sectors, buffer) != 0) {
        return; // bullshit params
    }

    // prepare read op
    if (!queued) {
        ATA_PREPARE_OP(drive, lba, sectors, ATA_CMD_READ);
    }

    // iterate through sectors
    SECTOR_ITERATE {
        // wait for drive to be ready
        ata_bs_wait();
        CATCH_DRIVE_ERR;

        // wait for data req
        ata_drq_wait();
        SECTOR_WORD_ITERATE {
            // read word
            ((uint16_t*) buffer)[j] = inw(ATA_PORT_DATA);
        }

        // move to next sector
        buffer += ATA_SECTOR_SIZE;
    }
}

void ata_write(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer, bool queued) {
    if (ata_op_validate(sectors, buffer) != 0) {
        return; // bs params
    }

    // prepare write op
    if (!queued) {
        ATA_PREPARE_OP(drive, lba, sectors, ATA_CMD_WRITE);
    }

    // iterate through sectors
    SECTOR_ITERATE {
        // wait for drive to be ready
        ata_bs_wait();
        CATCH_DRIVE_ERR;

        // wait for data req
        ata_drq_wait();
        SECTOR_WORD_ITERATE {
            // write word
            outw(ATA_PORT_DATA, ((uint16_t*) buffer)[j]);
        }

        // move to next sector
        buffer += ATA_SECTOR_SIZE;
    }
}

static ata_request_t* ata_queue_dequeue_unlocked(void) {
    if (!ata_queue.head) {
        return NULL; // empty queue
    }

    // fetch head
    ata_request_t* r = ata_queue.head;

    // move head to next req
    ata_queue.head = r->next;

    if (!ata_queue.head) {
        ata_queue.tail = NULL; // empty queue
    }

    r->next = NULL;
    ata_queue.length--;
    return r; // return dequeued request
}

static void ata_queue_enqueue_unlocked(ata_request_t* req) {
    req->next = NULL;

    if (!ata_queue.tail) {
        // first element
        ata_queue.head = ata_queue.tail = req;
    } else {
        // link to end
        ata_queue.tail->next = req;
        // update tail pointer
        ata_queue.tail = req;
    }

    ata_queue.length++;
}

// prepare a request
void ata_start_request(ata_request_t* req) {
    if (!req) {
        return;
    }

    switch (req->type) {
        case ATA_REQ_READ:
            ATA_PREPARE_OP(req->drive, req->lba, req->sector_count, ATA_CMD_READ);
            break;

        case ATA_REQ_WRITE:
            ATA_PREPARE_OP(req->drive, req->lba, req->sector_count, ATA_CMD_WRITE);
            break;

        case ATA_REQ_IDENTIFY:
            ATA_IDENTIFY_DRIVE(req->drive);
            break;

        default:
            // yeah we shouldn't be here
            break;
    }
}

// do the operation of a prepared request
int ata_finish_request(ata_request_t* req) {
    if (req->type == ATA_REQ_IDENTIFY) {
        uint16_t tmp[256];
        ID_WORD_ITERATE {
            // read id words
            tmp[i] = inw(ATA_PORT_DATA); // Note: Usually DATA port, check your macros
        }
        return 0;
    }

    if (req->type == ATA_REQ_READ) {
        ata_read(req->drive, req->lba, (uint8_t) req->sector_count, req->buffer, true);
        return 0;
    }

    if (req->type == ATA_REQ_WRITE) {
        ata_write(req->drive, req->lba, (uint8_t) req->sector_count, req->buffer, true);
        return 0;
    }

    // unknown request type
    log("ata: unknown request type\n", RED);
    return -1;
}

void ata_submit(ata_request_t* req) {
    spinlock(&ata_lock);

    bool was_empty = (ata_queue.head == NULL);
    ata_queue_enqueue_unlocked(req);

    if (was_empty) {
        // if queue empty, start req
        ata_start_request(req);
    }

    spinlock_unlock(&ata_lock, true);
}

void ata_irq_handler(void) {
    // acknowledge the irq immediately by reading the status register
    uint8_t status = inb(ATA_PORT_STATUS);

    spinlock(&ata_lock);

    // get current request from queue
    ata_request_t* req = ata_queue.head;

    if (req) {
        // do pio
        int st = ata_finish_request(req);

        ata_queue_dequeue_unlocked();

        if (req->completion) {
            // call completion callback
            req->completion(req, st);
        }

        // start next request if the queue isn't empty
        ata_request_t* next = ata_queue.head;
        if (next) {
            ata_start_request(next);
        }
    }

    spinlock_unlock(&ata_lock, true);
}

// detect drives, initialize them, and start the first request
// includes a timeout
void ata_init(void) {
    ata_queue_init();

    for (uint8_t drive = 0; drive < 2; drive++) {
        // check for floating bus (no controller/drives)
        if (inb(ATA_PORT_STATUS) == 0xFF) {
            continue;
        }

        ATA_DRIVE_INIT_PREPARE(drive);

        int timeout = 100000;
        while (--timeout && (inb(ATA_PORT_STATUS) & ATA_BSY)) {
            IA32_CPU_RELAX();
        }

        // if timeout is zero, drive is not present
        if (timeout == 0) {
            log_uint("ata: no ata drive present at index ", drive);
            continue; // skip this drive
        }

        // attempt to IDENTIFY using a temporary request
        ata_request_t identify_req;
        identify_req.drive = drive;
        identify_req.type = ATA_REQ_IDENTIFY;

        ATA_IDENTIFY_DRIVE(drive);

        // timeout for identify finish
        timeout = 100000;
        while (--timeout && !(inb(ATA_PORT_STATUS) & ATA_DRQ)) {
            // Check if drive dropped an error
            if (inb(ATA_PORT_STATUS) & ATA_ERR) {
                break;
            }
            IA32_CPU_RELAX();
        }

        if (timeout == 0 || (inb(ATA_PORT_STATUS) & ATA_ERR)) {
            log_uint("ata: identify failed/timeout for drive ", drive);
            continue; // skip this drive
        }

        ata_finish_request(&identify_req);

        uint8_t status = inb(ATA_PORT_STATUS);
        if (status == 0) {
            log_uint("ata: no ata drive present at index ", drive);
            continue;
        }

        log_uint("ata: found ata drive at index ", drive);
        ata_init_drive(drive);
    }

    log("ata: init - ok\n", GREEN);
}
