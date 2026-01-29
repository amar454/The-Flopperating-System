// SPDX-License-Identifier: GPL-3.0
#ifndef ACPI_H
#define ACPI_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    ACPI_SIG_RSDT = 0x54445352,
    ACPI_SIG_APIC = 0x43495041,
    ACPI_SIG_FACP = 0x50434146,
    ACPI_SIG_DSDT = 0x54445344,
    ACPI_SIG_S5 = 0x5F35535F,
    ACPI_SIG_PTS = 0x5354505F,
    ACPI_SIG_SST = 0x5453535F,
    ACPI_SIG_RSDP_L = 0x20445352,
    ACPI_SIG_RSDP_H = 0x20525450
} acpi_signature_t;

typedef enum {
    BIOS_ROM_START = 0x000E0000,
    BIOS_ROM_END = 0x00100000,
    EBDA_PTR_ADDR = 0x40E,
    EBDA_WINDOW_SIZE = 1024
} bios_mem_t;

typedef enum {
    ACPI_TABLE_HEAD_SIZE = 36,
    MADT_ENTRY_OFFSET = 44,
    MADT_TYPE_LAPIC = 0
} acpi_table_config_t;

typedef enum {
    ACPI_ENABLE_LOOP_MAX = 300,
    SLP_TYP_SHIFT = 10,
    SLP_EN_BIT = (1 << 13),
    SCI_EN_BIT = 1
} acpi_power_constants_t;

typedef enum {
    AML_OP_ZERO = 0x00,
    AML_OP_ONE = 0x01,
    AML_OP_ALIAS = 0x06,
    AML_OP_NAME = 0x08,
    AML_OP_BYTE_PREFIX = 0x0A,
    AML_OP_WORD_PREFIX = 0x0B,
    AML_OP_DWORD_PREFIX = 0x0C,
    AML_OP_STRING_PREFIX = 0x0D,
    AML_OP_QWORD_PREFIX = 0x0E,
    AML_OP_SCOPE = 0x10,
    AML_OP_BUFFER = 0x11,
    AML_OP_PACKAGE = 0x12,
    AML_OP_METHOD = 0x14,
    AML_OP_EXT_PREFIX = 0x5B,
    AML_OP_ONES = 0xFF,
    AML_EXT_MUTEX = 0x80,
    AML_EXT_EVENT = 0x81,
    AML_EXT_COND_REF = 0x82,
    AML_EXT_POWER_RES = 0x83
} aml_op_t;

typedef enum {
    QEMU_SHUTDOWN_PORT = 0x604,
    QEMU_SHUTDOWN_CMD = 0x2000
} qemu_constants_t;

typedef enum {
    TOKEN_INVALID = 0,
    TOKEN_RSD_PTR,
    TOKEN_RSDT,
    TOKEN_APIC,
    TOKEN_FACP,
    TOKEN_DSDT
} acpi_token_t;

// --- DATA STRUCTURES ---

typedef enum {
    ACPI_TYPE_INTEGER = 0,
    ACPI_TYPE_STRING,
    ACPI_TYPE_BUFFER,
    ACPI_TYPE_PACKAGE,
    ACPI_TYPE_METHOD,
    ACPI_TYPE_DEVICE,
    ACPI_TYPE_UNKNOWN
} acpi_object_type;

struct acpi_int {
    uint64_t value;
};

struct acpi_method {
    uint8_t* aml_start;
    uint32_t aml_len;
};

struct acpi_buffer {
    uint8_t* data;
    uint32_t len;
};

typedef struct acpi_object {
    union {
        struct acpi_int integer;
        struct acpi_method method;
        struct acpi_buffer buffer; // Used for both string and buffer
    };

    acpi_object_type type;
} acpi_object_t;

typedef struct acpi_ns_node {
    uint32_t name;
    acpi_object_t* obj;
    struct acpi_ns_node* parent;
    struct acpi_ns_node* children;
    struct acpi_ns_node* next;
} acpi_ns_node_t;

struct rsd_ptr {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed));

struct facp {
    char signature[4];
    uint32_t length;
    char unused1[32];
    uint32_t dsdt;
    char unused2[4];
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    char unused3[10];
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    char unused4[17];
    uint8_t pm1_cnt_len;
} __attribute__((packed));

int acpi_init(void);
void acpi_power_off(void);
void qemu_power_off(void);

#endif // ACPI_H
