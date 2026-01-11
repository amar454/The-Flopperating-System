#pragma once
#ifndef ATA_H
#define ATA_H
#include <stdint.h>
#include "../../task/sync/mutex.h"

typedef enum ata_port {
    ATA_PORT_BASE = 0x1F0,
    ATA_PORT_DATA = ATA_PORT_BASE + 0,
    ATA_PORT_ERROR = ATA_PORT_BASE + 1,
    ATA_PORT_FEATURES = ATA_PORT_BASE + 1,
    ATA_PORT_SECTOR_COUNT = ATA_PORT_BASE + 2,
    ATA_PORT_LBA_LOW = ATA_PORT_BASE + 3,
    ATA_PORT_LBA_MID = ATA_PORT_BASE + 4,
    ATA_PORT_LBA_HIGH = ATA_PORT_BASE + 5,
    ATA_PORT_DRIVE_HEAD = ATA_PORT_BASE + 6,
    ATA_PORT_STATUS = ATA_PORT_BASE + 7,
    ATA_PORT_COMMAND = ATA_PORT_BASE + 7
} ata_port_t;

typedef enum ata_cmd {
    ATA_CMD_READ = 0x20,
    ATA_CMD_WRITE = 0x30,
    ATA_CMD_IDENT = 0xEC
} ata_cmd_t;

#define ATA_ERR 0x01
#define ATA_BSY 0x80
#define ATA_DRQ 0x08
#define ATA_RDY 0x40
#define MAX_SECTORS 256
#define ATA_SECTOR_SIZE 512
#define SECTOR_ITERATE for (uint8_t i = 0; i < sectors; i++)

#define ATA_DRIVE_INIT_PREPARE(drive_num)                                                                              \
    do {                                                                                                               \
        outb(ATA_PORT_DRIVE_HEAD, (drive_num));                                                                        \
        outb(ATA_PORT_SECTOR_COUNT, 0);                                                                                \
        outb(ATA_PORT_LBA_LOW, 0);                                                                                     \
        outb(ATA_PORT_LBA_MID, 0);                                                                                     \
        outb(ATA_PORT_LBA_HIGH, 0);                                                                                    \
        outb(ATA_PORT_DRIVE_HEAD, 0);                                                                                  \
        outb(ATA_PORT_COMMAND, 0xEC);                                                                                  \
    } while (0)

#define ATA_PREPARE_OP(drive, lba, sectors, cmd)                                                                       \
    do {                                                                                                               \
        outb(ATA_PORT_DRIVE_HEAD, 0xE0 | ((drive) >> 4) | (((lba) >> 24) & 0x0F));                                     \
        outb(ATA_PORT_SECTOR_COUNT, (sectors));                                                                        \
        outb(ATA_PORT_LBA_LOW, (uint8_t) ((lba) >> 8));                                                                \
        outb(ATA_PORT_LBA_MID, (uint8_t) ((lba) >> 16));                                                               \
        outb(ATA_PORT_LBA_HIGH, ((lba) >> 16) & 0xFF);                                                                 \
        outb(ATA_PORT_COMMAND, (cmd));                                                                                 \
    } while (0)

#define ATA_IDENTIFY_DRIVE(drive)                                                                                      \
    do {                                                                                                               \
        outb(ATA_PORT_DRIVE_HEAD, (drive));                                                                            \
        outb(ATA_PORT_SECTOR_COUNT, 0);                                                                                \
        outb(ATA_PORT_LBA_LOW, 0);                                                                                     \
        outb(ATA_PORT_LBA_MID, 0);                                                                                     \
        outb(ATA_PORT_LBA_HIGH, 0);                                                                                    \
        outb(ATA_PORT_COMMAND, 0xEC);                                                                                  \
    } while (0)

#define SECTOR_WORD_ITERATE for (uint16_t j = 0; j < ATA_SECTOR_SIZE / 2; j++)
#define ID_WORD_ITERATE for (uint16_t i = 0; i < 256; i++)
#define ATA_PORT_DRQ (!(inb(ATA_PORT_STATUS) & ATA_DRQ))
#define ATA_PORT_BUSY (!(inb(ATA_PORT_STATUS) & ATA_BSY))
#define ATTEMPT_IDENTITY_REQUEST                                                                                       \
    identify_req.type = ATA_REQ_IDENTIFY;                                                                              \
    identify_req.drive = drive;                                                                                        \
    identify_req.lba = 0;                                                                                              \
    identify_req.sector_count = 0;                                                                                     \
    identify_req.buffer = NULL;                                                                                        \
    identify_req.next = NULL;                                                                                          \
    identify_req.completion = NULL;                                                                                    \
    ata_start_request(&identify_req);

#define CATCH_DRIVE_ERR                                                                                                \
    if (inb(ATA_PORT_STATUS) & 0x01) {                                                                                 \
        log_uint("ata error: ", inb(ATA_PORT_ERROR));                                                                  \
    }

typedef enum {
    ATA_REQ_READ,
    ATA_REQ_WRITE,
    ATA_REQ_IDENTIFY
} ata_req_type_t;

struct ata_request;
typedef void (*ata_completion_t)(struct ata_request* req, int status);

typedef struct ata_request {
    ata_req_type_t type;
    uint8_t drive;
    uint32_t lba;
    uint16_t sector_count;
    void* buffer;
    ata_completion_t completion;
    struct ata_request* next;
} ata_request_t;

typedef struct {
    ata_request_t* head;
    ata_request_t* tail;
    size_t length;
    spinlock_t lock;
} ata_queue_t;

extern ata_queue_t ata_queue;

void ata_init_drive(uint8_t drive_num);
void ata_read(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer, bool queued);
void ata_write(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer, bool queued);

void ata_queue_init(void);
void ata_submit(ata_request_t* req);
void ata_irq_handler(void);

void ata_start_request(ata_request_t* req);
int ata_finish_request(ata_request_t* req);

void ata_init(void);

#endif
