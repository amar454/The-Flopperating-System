#ifndef FLOPPERATING_ACPI_H
#define FLOPPERATING_ACPI_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    char sig[8];
    uint8_t checksum;
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) rsdp_t;

typedef struct {
    uint32_t sig;
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table[8];
    uint32_t oem_rev;
    uint32_t creator_id;
    uint32_t creator_rev;
} __attribute__((packed)) sdt_t;

typedef struct {
    sdt_t h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_len;
    uint8_t gpe1_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved2;
    uint32_t flags;
} __attribute__((packed)) fadt_t;

enum acpi_rsdp_signature_low {
    ACPI_RSDP_SIG_LO = 0x20525450
};

enum acpi_rsdp_signature_high {
    ACPI_RSDP_SIG_HI = 0x20445352
};

enum acpi_table_signatures {
    ACPI_SIG_RSDT = 0x54445352,
    ACPI_SIG_XSDT = 0x54445358,
    ACPI_SIG_FADT = 0x50434146,
    ACPI_SIG_APIC = 0x43495041,
    ACPI_SIG_DSDT = 0x54445344
};

enum acpi_pm1_bits {
    ACPI_PM1_SCI_EN = 1 << 0,
    ACPI_PM1_SLP_EN = 1 << 13
};

enum acpi_pm1_shift {
    ACPI_PM1_SLP_TYP_SHIFT = 10
};

enum acpi_madt_types {
    ACPI_MADT_TYPE_LAPIC = 0
};

enum acpi_scan_ranges {
    ACPI_SCAN_BIOS_START = 0xE0000,
    ACPI_SCAN_BIOS_END = 0x100000,
    ACPI_SCAN_EBDA_PTR = 0x40E,
    ACPI_SCAN_EBDA_SIZE = 1024
};

enum acpi_enable_limits {
    ACPI_ENABLE_TIMEOUT = 1000000
};

enum acpi_qemu_constants {
    ACPI_QEMU_PORT = 0x604,
    ACPI_QEMU_CMD = 0x2000
};

int acpi_init(void);
void acpi_power_off(void);
void acpi_qemu_power_off(void);

#endif
