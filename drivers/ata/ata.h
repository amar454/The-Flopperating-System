#ifndef ATA_H
#define ATA_H
#include <stdint.h>

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
    ATA_CMD_WRITE = 0x30
} ata_cmd_t;

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
        outb(ATA_PORT_COMMAND, cmd);                                                                                   \
    } while (0)

#define ATA_IDENTIFY_DRIVE(drive)                                                                                      \
    do {                                                                                                               \
        outb(ATA_PORT_DRIVE_HEAD, drive);                                                                              \
        outb(ATA_PORT_SECTOR_COUNT, 0);                                                                                \
        outb(ATA_PORT_LBA_LOW, 0);                                                                                     \
        outb(ATA_PORT_LBA_MID, 0);                                                                                     \
        outb(ATA_PORT_LBA_HIGH, 0);                                                                                    \
        outb(ATA_PORT_COMMAND, 0xEC);                                                                                  \
    } while (0)

#define ATA_BSY 0x80
#define ATA_DRQ 0x08
#define MAX_SECTORS 256
#define ATA_SECTOR_SIZE 512
#define SECTOR_ITERATE for (uint8_t i = 0; i < sectors; i++)

void ata_init_drive(uint8_t drive_num);
void ata_read(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer);
void ata_write(uint8_t drive, uint32_t lba, uint8_t sectors, uint8_t* buffer);

#endif /* ATA_H */
