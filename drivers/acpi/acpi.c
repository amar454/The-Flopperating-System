#include "acpi.h"
#include "../vga/vgahandler.h"
#include "../../lib/logging.h"
#include "../../mem/utils.h"
#include "../../mem/early.h"
#include "../io/io.h"
#include "../time/floptime.h"
#include "../../task/smp.h"

static uint32_t* smi_cmd;
static uint8_t acpi_enable_val;
static uint8_t acpi_disable_val;
static uint32_t* pm1a_cnt;
static uint32_t* pm1b_cnt;
static uint16_t slp_typa;
static uint16_t slp_typb;
static uint16_t slp_en;
static uint16_t sci_en;
static uint8_t pm1_cnt_len;

static uint8_t* dsdt_addr;
static uint32_t dsdt_len;
static acpi_ns_node_t* acpi_root_node;

static acpi_token_t acpi_tokenize(const void* ptr) {
    uint32_t sig32 = *(const uint32_t*) ptr;
    switch (sig32) {
        case ACPI_SIG_RSDT:
            return TOKEN_RSDT;
        case ACPI_SIG_APIC:
            return TOKEN_APIC;
        case ACPI_SIG_FACP:
            return TOKEN_FACP;
        case ACPI_SIG_DSDT:
            return TOKEN_DSDT;
        case ACPI_SIG_RSDP_L:
            if (*((const uint32_t*) ptr + 1) == ACPI_SIG_RSDP_H) {
                return TOKEN_RSD_PTR;
            }
            break;
    }
    return TOKEN_INVALID;
}

static uint32_t aml_get_pkglen(uint8_t* ptr, int* bytes_read) {
    uint8_t b0 = *ptr;
    uint8_t count = (b0 >> 6);
    uint32_t len = (b0 & 0x3F);
    if (count == 0) {
        *bytes_read = 1;
        return len;
    }
    len = (b0 & 0x0F);
    for (int i = 0; i < count; i++) {
        len |= ((uint32_t) ptr[1 + i] << (4 + i * 8));
    }
    *bytes_read = 1 + count;
    return len;
}

static uint8_t* aml_skip_object(uint8_t* ptr) {
    uint8_t op = *ptr;
    int bytes;
    uint32_t len;

    switch (op) {
        case AML_OP_EXT_PREFIX:
            switch (ptr[1]) {
                case AML_EXT_MUTEX:
                case AML_EXT_EVENT:
                case AML_EXT_COND_REF:
                case AML_EXT_POWER_RES:
                    len = aml_get_pkglen(ptr + 2, &bytes);
                    return (ptr + 2) + len;
                default:
                    return ptr + 1;
            }
        case AML_OP_SCOPE:
        case AML_OP_BUFFER:
        case AML_OP_PACKAGE:
        case AML_OP_METHOD:
            len = aml_get_pkglen(ptr + 1, &bytes);
            return (ptr + 1) + len;
        case AML_OP_NAME:
            return aml_skip_object(ptr + 5);
        case AML_OP_BYTE_PREFIX:
            return ptr + 2;
        case AML_OP_WORD_PREFIX:
            return ptr + 3;
        case AML_OP_DWORD_PREFIX:
            return ptr + 5;
        case AML_OP_QWORD_PREFIX:
            return ptr + 9;
        case AML_OP_STRING_PREFIX:
            ptr++;
            while (*ptr++) {
            }
            return ptr;
        case AML_OP_ZERO:
        case AML_OP_ONE:
        case AML_OP_ONES:
            return ptr + 1;
        case AML_OP_ALIAS:
            return aml_skip_object(ptr + 1);
        default:
            return ptr + 1;
    }
}

static uint8_t* aml_parse_int(uint8_t* ptr, uint32_t* value) {
    uint8_t op = *ptr++;
    switch (op) {
        case AML_OP_ZERO:
            *value = 0;
            return ptr;
        case AML_OP_ONE:
            *value = 1;
            return ptr;
        case AML_OP_BYTE_PREFIX:
            *value = *ptr++;
            return ptr;
        case AML_OP_WORD_PREFIX:
            *value = *(uint16_t*) ptr;
            return ptr + 2;
        case AML_OP_DWORD_PREFIX:
            *value = *(uint32_t*) ptr;
            return ptr + 4;
        default:
            *value = 0;
            return ptr;
    }
}

static acpi_ns_node_t* acpi_create_node(uint32_t name, acpi_ns_node_t* parent) {
    acpi_ns_node_t* node = (acpi_ns_node_t*) early_alloc(sizeof(acpi_ns_node_t));
    if (node) {
        node->name = name;
        node->parent = parent;
        node->children = NULL;
        node->next = NULL;
        node->obj = NULL;

        if (parent) {
            if (parent->children == NULL) {
                parent->children = node;
            } else {
                acpi_ns_node_t* sibling = parent->children;
                while (sibling->next) {
                    sibling = sibling->next;
                }
                sibling->next = node;
            }
        }
    }
    return node;
}

static void acpi_attach_method(acpi_ns_node_t* node, uint8_t* aml_start, uint32_t aml_len) {
    if (!node) {
        return;
    }

    acpi_object_t* obj = (acpi_object_t*) early_alloc(sizeof(acpi_object_t));
    if (obj) {
        obj->type = ACPI_TYPE_METHOD;
        obj->method.aml_start = aml_start;
        obj->method.aml_len = aml_len;
        node->obj = obj;
    }
}

static void acpi_build_namespace(uint8_t* ptr, uint8_t* end, acpi_ns_node_t* parent) {
    while (ptr < end) {
        uint8_t op = *ptr;
        int pkg_bytes = 0;
        uint32_t pkg_len = 0;
        uint32_t name = 0;

        switch (op) {
            case AML_OP_SCOPE: {
                pkg_len = aml_get_pkglen(ptr + 1, &pkg_bytes);
                name = *(uint32_t*) (ptr + 1 + pkg_bytes);

                acpi_ns_node_t* node = acpi_create_node(name, parent);
                if (node) {
                    acpi_build_namespace(ptr + 1 + pkg_bytes + 4, ptr + 1 + pkg_len, node);
                }
                ptr += (1 + pkg_len);
                break;
            }
            case AML_OP_EXT_PREFIX: {
                if (ptr[1] == 0x82) {
                    pkg_len = aml_get_pkglen(ptr + 2, &pkg_bytes);
                    name = *(uint32_t*) (ptr + 2 + pkg_bytes);

                    acpi_ns_node_t* node = acpi_create_node(name, parent);
                    if (node) {
                        acpi_object_t* obj = (acpi_object_t*) early_alloc(sizeof(acpi_object_t));
                        if (obj) {
                            obj->type = ACPI_TYPE_DEVICE;
                            node->obj = obj;
                        }

                        acpi_build_namespace(ptr + 2 + pkg_bytes + 4, ptr + 2 + pkg_len, node);
                    }
                    ptr += (2 + pkg_len);
                } else {
                    ptr = aml_skip_object(ptr);
                }
                break;
            }
            case AML_OP_NAME:
                name = *(uint32_t*) (ptr + 1);
                acpi_create_node(name, parent);
                ptr += 5;
                ptr = aml_skip_object(ptr);
                break;
            case AML_OP_METHOD:
                pkg_len = aml_get_pkglen(ptr + 1, &pkg_bytes);
                name = *(uint32_t*) (ptr + 1 + pkg_bytes);

                acpi_ns_node_t* node = acpi_create_node(name, parent);
                if (node) {
                    uint8_t* body_start = ptr + 1 + pkg_bytes + 4 + 1;
                    uint32_t body_len = pkg_len - (pkg_bytes + 5);
                    acpi_attach_method(node, body_start, body_len);
                }
                ptr += (1 + pkg_len);
                break;
            default:
                ptr = aml_skip_object(ptr);
                break;
        }
    }
}

static uint8_t* acpi_resolve_object_linear(uint32_t signature) {
    uint8_t* ptr = dsdt_addr + ACPI_TABLE_HEAD_SIZE;
    uint8_t* end = dsdt_addr + dsdt_len;

    while (ptr < end) {
        uint8_t op = *ptr;
        switch (op) {
            case AML_OP_NAME:
                if (*(uint32_t*) (ptr + 1) == signature) {
                    return ptr + 5;
                }
                ptr += 5;
                ptr = aml_skip_object(ptr);
                break;
            case AML_OP_SCOPE:
            case AML_OP_METHOD:
            case AML_OP_PACKAGE: {
                int bytes;
                uint32_t len = aml_get_pkglen(ptr + 1, &bytes);
                ptr = (ptr + 1) + len;
                break;
            }
            default:
                if (op == AML_OP_EXT_PREFIX || op == AML_OP_BYTE_PREFIX || op == AML_OP_WORD_PREFIX ||
                    op == AML_OP_DWORD_PREFIX || op == AML_OP_STRING_PREFIX) {
                    ptr = aml_skip_object(ptr);
                } else {
                    ptr++;
                }
                break;
        }
    }
    return NULL;
}

static acpi_ns_node_t* acpi_find_node_recursive(acpi_ns_node_t* root, uint32_t name) {
    if (!root) {
        return NULL;
    }

    if (root->name == name) {
        return root;
    }

    acpi_ns_node_t* child = root->children;
    while (child) {
        acpi_ns_node_t* found = acpi_find_node_recursive(child, name);

        if (found) {
            return found;
        }

        child = child->next;
    }
    return NULL;
}

static void acpi_execute_method(uint32_t signature, uint32_t arg) {
    acpi_ns_node_t* node = acpi_find_node_recursive(acpi_root_node, signature);

    if (node && node->obj && node->obj->type == ACPI_TYPE_METHOD) {
        log_uint("acpi: Executing method from namespace: ", signature);
        return;
    }

    uint8_t* obj = acpi_resolve_object_linear(signature);
    if (obj) {
        log_uint("acpi: Executing method linear scan: ", signature);
    }
}

static int acpi_eval_s5(void) {
    uint8_t* ptr = dsdt_addr + ACPI_TABLE_HEAD_SIZE;
    uint8_t* end = dsdt_addr + dsdt_len;

    while (ptr < end) {
        if (*ptr == AML_OP_NAME && *(uint32_t*) (ptr + 1) == ACPI_SIG_S5) {
            uint8_t* val = ptr + 5;
            if (*val == AML_OP_PACKAGE) {
                int pkg_bytes;
                aml_get_pkglen(val + 1, &pkg_bytes);
                uint8_t* contents = val + 1 + pkg_bytes;
                uint8_t num_elements = *contents++;

                if (num_elements >= 2) {
                    uint32_t a = 0, b = 0;
                    contents = aml_parse_int(contents, &a);
                    contents = aml_parse_int(contents, &b);
                    slp_typa = a << SLP_TYP_SHIFT;
                    slp_typb = b << SLP_TYP_SHIFT;
                    slp_en = SLP_EN_BIT;
                    return 0;
                }
            }
        }
        ptr++;
    }
    return -1;
}

static uint32_t* acpi_check_rsdp(uint32_t* ptr) {
    struct rsd_ptr* rsdp = (struct rsd_ptr*) ptr;
    uint8_t* bptr;
    uint8_t check = 0;

    if (acpi_tokenize(rsdp) == TOKEN_RSD_PTR) {
        bptr = (uint8_t*) ptr;
        for (size_t i = 0; i < sizeof(struct rsd_ptr); i++) {
            check += *bptr++;
        }
        if (check == 0) {
            return (uint32_t*) rsdp->rsdt_address;
        }
    }
    return NULL;
}

static uint32_t* acpi_get_rsdp(void) {
    uint32_t* addr;
    uint32_t* rsdp;

    for (addr = (uint32_t*) BIOS_ROM_START; (uintptr_t) addr < BIOS_ROM_END; addr += 4) {
        rsdp = acpi_check_rsdp(addr);
        if (rsdp != NULL) {
            return rsdp;
        }
    }

    uint32_t ebda = *((uint16_t*) EBDA_PTR_ADDR);
    ebda = (ebda * 0x10) & 0x000FFFFF;
    for (addr = (uint32_t*) ebda; (uintptr_t) addr < ebda + EBDA_WINDOW_SIZE; addr += 4) {
        rsdp = acpi_check_rsdp(addr);
        if (rsdp != NULL) {
            return rsdp;
        }
    }
    return NULL;
}

static int acpi_check_header(uint32_t* ptr, acpi_token_t expected_token) {
    if (acpi_tokenize(ptr) == expected_token) {
        char* check_ptr = (char*) ptr;
        uint32_t len = *(ptr + 1);
        uint8_t check = 0;
        while (len-- > 0) {
            check += *check_ptr++;
        }
        return (check == 0) ? 0 : -1;
    }
    return -1;
}

static void acpi_parse_madt(uintptr_t madt_ptr) {
    uint32_t length = *(uint32_t*) (madt_ptr + 4);
    uintptr_t end = madt_ptr + length;
    uintptr_t current = madt_ptr + MADT_ENTRY_OFFSET;

    while (current < end) {
        uint8_t type = *(uint8_t*) (current);
        uint8_t entry_len = *(uint8_t*) (current + 1);
        if (type == MADT_TYPE_LAPIC) {
            uint8_t processor_id = *(uint8_t*) (current + 2);
            uint8_t apic_id = *(uint8_t*) (current + 3);
            uint32_t flags = *(uint32_t*) (current + 4);
            if ((flags & 1) != 0 && cpu_count < MAX_CPUS) {
                cpus[cpu_count].lapic_id = apic_id;
                cpus[cpu_count].acpi_id = processor_id;
                cpu_count++;
            }
        }
        current += entry_len;
    }
    log_uint("acpi: cpu count via MADT: ", cpu_count);
}

static int acpi_enable_native(void) {
    if ((inw((uintptr_t) pm1a_cnt) & sci_en) == 0) {
        if (smi_cmd && acpi_enable_val) {
            outb((uintptr_t) smi_cmd, acpi_enable_val);
            for (int i = 0; i < ACPI_ENABLE_LOOP_MAX; i++) {
                if ((inw((uintptr_t) pm1a_cnt) & sci_en) == SCI_EN_BIT) {
                    break;
                }
            }
            if (pm1b_cnt) {
                for (int i = 0; i < ACPI_ENABLE_LOOP_MAX; i++) {
                    if ((inw((uintptr_t) pm1b_cnt) & sci_en) == SCI_EN_BIT) {
                        break;
                    }
                }
            }
            log("ACPI enabled.", GREEN);
            return 0;
        }
        return -1;
    }
    return 0;
}

int acpi_init(void) {
    uint32_t* rsdt_ptr = acpi_get_rsdp();

    if (rsdt_ptr && acpi_check_header(rsdt_ptr, TOKEN_RSDT) == 0) {
        int entries = (*(rsdt_ptr + 1) - ACPI_TABLE_HEAD_SIZE) / 4;
        uint32_t* entry_ptr = rsdt_ptr + 9;

        while (entries-- > 0) {
            uintptr_t table_addr = *entry_ptr;

            if (acpi_check_header((uint32_t*) table_addr, TOKEN_APIC) == 0) {
                acpi_parse_madt(table_addr);
            }

            if (acpi_check_header((uint32_t*) table_addr, TOKEN_FACP) == 0) {
                struct facp* f = (struct facp*) table_addr;

                smi_cmd = (uint32_t*) f->smi_cmd;
                acpi_enable_val = f->acpi_enable;
                acpi_disable_val = f->acpi_disable;
                pm1a_cnt = (uint32_t*) f->pm1a_cnt_blk;
                pm1b_cnt = (uint32_t*) f->pm1b_cnt_blk;
                pm1_cnt_len = f->pm1_cnt_len;
                sci_en = 1;

                dsdt_addr = (uint8_t*) f->dsdt;
                dsdt_len = *(uint32_t*) (dsdt_addr + 1) - ACPI_TABLE_HEAD_SIZE;

                if (acpi_check_header((uint32_t*) dsdt_addr, TOKEN_DSDT) == 0) {
                    log("acpi: DSDT found.", GREEN);

                    acpi_root_node = acpi_create_node(0, NULL);
                    if (acpi_root_node) {
                        acpi_build_namespace(dsdt_addr + ACPI_TABLE_HEAD_SIZE,
                                             dsdt_addr + dsdt_len + ACPI_TABLE_HEAD_SIZE,
                                             acpi_root_node);
                        log("acpi: namespace built.", GREEN);
                    }

                    if (acpi_eval_s5() == 0) {
                        log("acpi: _S5 evaluated successfully.", GREEN);
                    }
                }
            }
            entry_ptr++;
        }
        log("acpi: init - ok", GREEN);
        return 0;
    }
    log("acpi: no acpi detected.", RED);
    return -1;
}

void acpi_power_off(void) {
    if (!sci_en) {
        return;
    }

    acpi_enable_native();
    acpi_execute_method(ACPI_SIG_PTS, 5);
    acpi_execute_method(ACPI_SIG_SST, 1);

    outw((uintptr_t) pm1a_cnt, slp_typa | slp_en);
    if (pm1b_cnt) {
        outw((uintptr_t) pm1b_cnt, slp_typb | slp_en);
    }
}

void qemu_power_off() {
    outw(QEMU_SHUTDOWN_PORT, QEMU_SHUTDOWN_CMD);
}
