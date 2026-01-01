#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../io/io.h"
#include "../../lib/logging.h"
#include "ata.h"
#include "../../interrupts/interrupts.h"
extern thread_t* current_thread;

ata_queue_t ata_queue;

static void ata_bsy_wait(void) {
    while (!(inb(ATA_PORT_STATUS) & ATA_BSY)) {
        IA32_CPU_RELAX();
    }
}

static void ata_drq_wait(void) {
    while (!(inb(ATA_PORT_STATUS) & ATA_DRQ))
        ;
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
    mutex_init(&ata_queue.lock, "ata_queue_lock");
}

// read sectors into buffer
void ata_read(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer) {
    if (ata_op_validate(sectors, buffer) != 0) {
        return; // bullshit params
    }

    // prepare read op
    ATA_PREPARE_OP(drive, lba, sectors, ATA_CMD_READ);

    // iterate through sectors
    SECTOR_ITERATE {
        // wait for drive to be ready
        ata_bsy_wait();
        if (inb(ATA_PORT_STATUS) & 0x01) {
            log_uint("ata error: ", inb(ATA_PORT_ERROR));
        }

        // wait for data req
        ata_drq_wait();
        for (uint16_t j = 0; j < ATA_SECTOR_SIZE / 2; j++) {
            // read word
            ((uint16_t*) buffer)[j] = inw(ATA_PORT_DATA);
        }

        // move to next sector
        buffer += ATA_SECTOR_SIZE;
    }
}

void ata_write(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer) {
    if (ata_op_validate(sectors, buffer) != 0) {
        return; // bs params
    }

    // prepare write op
    ATA_PREPARE_OP(drive, lba, sectors, ATA_CMD_WRITE);

    // iterate through sectors
    SECTOR_ITERATE {
        // wait for drive to be ready
        ata_bsy_wait();
        if (inb(ATA_PORT_STATUS) & 0x01) {
            log_uint("ata error: ", inb(ATA_PORT_ERROR));
        }

        // wait for data req
        ata_drq_wait();
        for (uint16_t j = 0; j < ATA_SECTOR_SIZE / 2; j++) {
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
        for (uint16_t i = 0; i < 256; i++) {
            // read id words
            tmp[i] = inw(ATA_PORT_BASE);
        }
        return 0;
    }

    if (req->type == ATA_REQ_READ) {
        ata_read(req->drive, req->lba, (uint8_t) req->sector_count, req->buffer);
        return 0;
    }

    if (req->type == ATA_REQ_WRITE) {
        ata_write(req->drive, req->lba, (uint8_t) req->sector_count, req->buffer);
        return 0;
    }

    // unknown request type
    log("ata: unknown request type", RED);
    return -1;
}

void ata_submit(ata_request_t* req) {
    mutex_lock(&ata_queue.lock, current_thread);

    ata_queue_enqueue_unlocked(req);

    if (ata_queue.length == 1) {
        // if queue empty, start req
        ata_start_request(req);
    }

    mutex_unlock(&ata_queue.lock);
}

void ata_irq_handler(void) {
    // try to grab the mutex without blocking
    int expected = MUTEX_UNLOCKED;

    if (!atomic_compare_exchange_strong(&ata_queue.lock.state, &expected, MUTEX_LOCKED)) {
        // someone else owns the queue
        return;
    }

    ata_queue.lock.owner = current_thread;

    // get next request from queue
    ata_request_t* req = ata_queue_dequeue_unlocked();

    if (req) {
        // do pio
        int st = ata_finish_request(req);
        if (req->completion) {
            // call completion callback
            req->completion(req, st);
        }

        ata_request_t* next = ata_queue.head;
        if (next) {
            // start next request if applicable
            ata_start_request(next);
        }
    }

    mutex_unlock(&ata_queue.lock);
}

// detect drives, initialize them, and start the first request
// includes a timeout
void ata_init(void) {
    ata_queue_init();

    for (uint8_t drive = 0; drive < 2; drive++) {
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
        identify_req.type = ATA_REQ_IDENTIFY;
        identify_req.drive = drive;
        identify_req.lba = 0;
        identify_req.sector_count = 0;
        identify_req.buffer = NULL;
        identify_req.next = NULL;
        identify_req.completion = NULL;

        ata_start_request(&identify_req);

        // add timeout for identify finish
        timeout = 100000;
        while (--timeout && !(inb(ATA_PORT_STATUS) & ATA_DRQ)) {
            IA32_CPU_RELAX();
        }

        if (timeout == 0) {
            log_uint("ata: identify timeout for drive ", drive);
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

    log("ata: init - ok", GREEN);
}
