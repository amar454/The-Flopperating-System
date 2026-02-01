#include "acpi.h"
#include "../io/io.h"
#include "../../lib/logging.h"

static fadt_t* g_fadt;
static uint16_t g_slp_typa;
static uint16_t g_slp_typb;

static int acpi_checksum(void* ptr, size_t len) {
    uint8_t sum = 0;
    uint8_t* p = ptr;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return sum == 0;
}

static rsdp_t* acpi_find_rsdp(void) {
    for (uintptr_t p = ACPI_SCAN_BIOS_START; p < ACPI_SCAN_BIOS_END; p += 16) {
        rsdp_t* r = (rsdp_t*) p;
        if (*(uint32_t*) r->sig == ACPI_RSDP_SIG_LO && *(uint32_t*) (r->sig + 4) == ACPI_RSDP_SIG_HI &&
            acpi_checksum(r, 20)) {
            if (r->revision >= 2) {
                if (!acpi_checksum(r, r->length)) {
                    continue;
                }
            }
            return r;
        }
    }

    uint16_t ebda_seg = *(uint16_t*) ACPI_SCAN_EBDA_PTR;
    uintptr_t ebda = ebda_seg << 4;

    for (uintptr_t p = ebda; p < ebda + ACPI_SCAN_EBDA_SIZE; p += 16) {
        rsdp_t* r = (rsdp_t*) p;
        if (*(uint32_t*) r->sig == ACPI_RSDP_SIG_LO && *(uint32_t*) (r->sig + 4) == ACPI_RSDP_SIG_HI &&
            acpi_checksum(r, 20)) {
            if (r->revision >= 2) {
                if (!acpi_checksum(r, r->length)) {
                    continue;
                }
            }
            return r;
        }
    }

    return NULL;
}

static void acpi_parse_madt(sdt_t* madt) {
    uintptr_t ptr = (uintptr_t) madt + sizeof(sdt_t) + 8;
    uintptr_t end = (uintptr_t) madt + madt->length;

    while (ptr < end) {
        uint8_t type = *(uint8_t*) ptr;
        uint8_t len = *(uint8_t*) (ptr + 1);

        if (type == ACPI_MADT_TYPE_LAPIC) {
            uint32_t flags = *(uint32_t*) (ptr + 4);
            if (flags & 1) {
                log("acpi: cpu online", GREEN);
            }
        }

        ptr += len;
    }
}

static void acpi_parse_s5(uint8_t* dsdt, uint32_t len) {
    for (uint32_t i = 0; i < len - 4; i++) {
        if (*(uint32_t*) (dsdt + i) == 0x35535F) {
            uint8_t* p = dsdt + i + 4;
            if (*p == 0x12) {
                uint8_t count = p[2];
                uint8_t* data = p + 3;
                if (count >= 2) {
                    g_slp_typa = data[1] << ACPI_PM1_SLP_TYP_SHIFT;
                    g_slp_typb = data[3] << ACPI_PM1_SLP_TYP_SHIFT;
                    return;
                }
            }
        }
    }
}

int acpi_init(void) {
    log("acpi: finding rsdp\n", LIGHT_GRAY);
    rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) {
        return -1;
    }

    sdt_t* root;
    int entries;

    if (rsdp->revision >= 2 && rsdp->xsdt) {
        root = (sdt_t*) (uintptr_t) rsdp->xsdt;
        entries = (root->length - sizeof(sdt_t)) / 8;
        uint64_t* e = (uint64_t*) ((uintptr_t) root + sizeof(sdt_t));
        log("acpi: iterating through entries\n", LIGHT_GRAY);
        for (int i = 0; i < entries; i++) {
            sdt_t* t = (sdt_t*) (uintptr_t) e[i];
            if (!acpi_checksum(t, t->length)) {
                continue;
            }
            if (t->sig == ACPI_SIG_FADT) {
                log("acpi: found fadt\n", LIGHT_GRAY);
                g_fadt = (fadt_t*) t;
            }
            if (t->sig == ACPI_SIG_APIC) {
                log("acpi: found apic\n", LIGHT_GRAY);
                acpi_parse_madt(t);
            }
        }
    } else {
        root = (sdt_t*) (uintptr_t) rsdp->rsdt;
        entries = (root->length - sizeof(sdt_t)) / 4;
        uint32_t* e = (uint32_t*) ((uintptr_t) root + sizeof(sdt_t));
        log("acpi: iterating through entries\n", LIGHT_GRAY);
        for (int i = 0; i < entries; i++) {
            sdt_t* t = (sdt_t*) (uintptr_t) e[i];
            if (!acpi_checksum(t, t->length)) {
                continue;
            }
            if (t->sig == ACPI_SIG_FADT) {
                g_fadt = (fadt_t*) t;
            }
            if (t->sig == ACPI_SIG_APIC) {
                acpi_parse_madt(t);
            }
        }
    }

    if (!g_fadt) {
        return -1;
    }

    sdt_t* dsdt = (sdt_t*) (uintptr_t) g_fadt->dsdt;
    if (!acpi_checksum(dsdt, dsdt->length)) {
        return -1;
    }

    log("acpi: parsing s5\n", LIGHT_GRAY);
    acpi_parse_s5((uint8_t*) dsdt + sizeof(sdt_t), dsdt->length - sizeof(sdt_t));

    return 0;
}

void acpi_power_off(void) {
    if (!g_fadt) {
        return;
    }

    if (!(inw(g_fadt->pm1a_cnt_blk) & ACPI_PM1_SCI_EN)) {
        if (g_fadt->smi_cmd && g_fadt->acpi_enable) {
            outb(g_fadt->smi_cmd, g_fadt->acpi_enable);
            for (volatile int i = 0; i < ACPI_ENABLE_TIMEOUT; i++) {
                if (inw(g_fadt->pm1a_cnt_blk) & ACPI_PM1_SCI_EN) {
                    break;
                }
            }
        }
    }

    outw(g_fadt->pm1a_cnt_blk, g_slp_typa | ACPI_PM1_SLP_EN);

    if (g_fadt->pm1b_cnt_blk) {
        outw(g_fadt->pm1b_cnt_blk, g_slp_typb | ACPI_PM1_SLP_EN);
    }
}

void acpi_qemu_power_off(void) {
    outw(ACPI_QEMU_PORT, ACPI_QEMU_CMD);
}
