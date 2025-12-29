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
        return -1;
    }
    if (sectors > (uint8_t) MAX_SECTORS) {
        return -1;
    }
    return 0;
}

void ata_init_drive(uint8_t drive_num) {
    ATA_DRIVE_INIT_PREPARE(drive_num);
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

void ata_read(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer) {
    if (ata_op_validate(sectors, buffer) != 0) {
        return;
    }
    ATA_PREPARE_OP(drive, lba, sectors, ATA_CMD_READ);
    SECTOR_ITERATE {
        ata_bsy_wait();
        if (inb(ATA_PORT_STATUS) & 0x01) {
            log_uint("ata error: ", inb(ATA_PORT_ERROR));
        }
        ata_drq_wait();
        for (uint16_t j = 0; j < ATA_SECTOR_SIZE / 2; j++) {
            ((uint16_t*) buffer)[j] = inw(ATA_PORT_DATA);
        }
        buffer += ATA_SECTOR_SIZE;
    }
}

void ata_write(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer) {
    if (ata_op_validate(sectors, buffer) != 0) {
        return;
    }
    ATA_PREPARE_OP(drive, lba, sectors, ATA_CMD_WRITE);
    SECTOR_ITERATE {
        ata_bsy_wait();
        if (inb(ATA_PORT_STATUS) & 0x01) {
            log_uint("ata error: ", inb(ATA_PORT_ERROR));
        }
        ata_drq_wait();
        for (uint16_t j = 0; j < ATA_SECTOR_SIZE / 2; j++) {
            outw(ATA_PORT_DATA, ((uint16_t*) buffer)[j]);
        }
        buffer += ATA_SECTOR_SIZE;
    }
}

static ata_request_t* ata_queue_dequeue_unlocked(void) {
    if (!ata_queue.head) {
        return NULL;
    }

    ata_request_t* r = ata_queue.head;
    ata_queue.head = r->next;

    if (!ata_queue.head) {
        ata_queue.tail = NULL;
    }

    r->next = NULL;
    ata_queue.length--;
    return r;
}

static void ata_queue_enqueue_unlocked(ata_request_t* req) {
    req->next = NULL;

    if (!ata_queue.tail) {
        ata_queue.head = ata_queue.tail = req;
    } else {
        ata_queue.tail->next = req;
        ata_queue.tail = req;
    }

    ata_queue.length++;
}

void ata_start_request(ata_request_t* req) {
    if (req->type == ATA_REQ_READ) {
        ATA_PREPARE_OP(req->drive, req->lba, req->sector_count, ATA_CMD_READ);
    } else if (req->type == ATA_REQ_WRITE) {
        ATA_PREPARE_OP(req->drive, req->lba, req->sector_count, ATA_CMD_WRITE);
    } else if (req->type == ATA_REQ_IDENTIFY) {
        ATA_IDENTIFY_DRIVE(req->drive);
    }
}

int ata_finish_request(ata_request_t* req) {
    if (req->type == ATA_REQ_IDENTIFY) {
        uint16_t tmp[256];
        for (uint16_t i = 0; i < 256; i++) {
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

    return -1;
}

void ata_submit(ata_request_t* req) {
    mutex_lock(&ata_queue.lock, current_thread);

    ata_queue_enqueue_unlocked(req);

    if (ata_queue.length == 1) {
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

    ata_request_t* req = ata_queue_dequeue_unlocked();

    if (req) {
        int st = ata_finish_request(req);
        if (req->completion) {
            req->completion(req, st);
        }

        ata_request_t* next = ata_queue.head;
        if (next) {
            ata_start_request(next);
        }
    }

    mutex_unlock(&ata_queue.lock);
}
