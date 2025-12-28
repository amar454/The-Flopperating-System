#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../io/io.h"
#include "../../lib/logging.h"
#include "ata.h"

void ata_init_drive(uint8_t drive_num) {
    ATA_DRIVE_INIT_PREPARE(drive_num);

    uint8_t status;
    status = inb(ATA_PORT_STATUS);

    // wait for drive to be ready
    while ((inb(ATA_PORT_STATUS) & ATA_BSY) == 0) {
        status = inb(ATA_PORT_STATUS);
    }
}

static void ata_bsy_wait(void) {
    while (!(inb(ATA_PORT_STATUS) & ATA_BSY))
        ;
}

static void ata_drq_wait(void) {
    while (!(inb(ATA_PORT_STATUS) & ATA_DRQ))
        ;
}

int ata_op_validate(uint8_t sectors, uint8_t* buffer) {
    if (!buffer || sectors == 0) {
        return -1;
    }

    if (sectors > (uint8_t) MAX_SECTORS) {
        return -1;
    }

    return 0;
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
            ((uint16_t*) buffer)[j] = inb(ATA_PORT_DATA);
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
            outb(ATA_PORT_DATA, ((uint16_t*) buffer)[j]);
        }

        buffer += ATA_SECTOR_SIZE;
    }
}

#define ATA_IDENTIFY_DRIVE(drive)                                                                                      \
    do {                                                                                                               \
        outb(ATA_PORT_DRIVE_HEAD, drive);                                                                              \
        outb(ATA_PORT_SECTOR_COUNT, 0);                                                                                \
        outb(ATA_PORT_LBA_LOW, 0);                                                                                     \
        outb(ATA_PORT_LBA_MID, 0);                                                                                     \
        outb(ATA_PORT_LBA_HIGH, 0);                                                                                    \
        outb(ATA_PORT_COMMAND, 0xEC);                                                                                  \
    } while (0)

void ata_id(uint8_t drive) {
    ATA_IDENTIFY_DRIVE(drive);

    ata_bsy_wait();

    uint8_t status = inb(ATA_PORT_BASE);

    if (status == 0) {
        log("ata: no device detected", RED);
        return;
    }

    uint16_t buffer[256];

    for (uint16_t i = 0; i < 256; i++) {
        buffer[i] = inw(ATA_PORT_BASE);
    }

    log("ata: device detected", GREEN);
}
