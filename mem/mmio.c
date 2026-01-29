#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint8_t mmio_read8(uintptr_t addr) {
    uint8_t val;
    __asm__ volatile("movb (%1), %%al" : "=a"(val) : "r"(addr) : "memory");
    return val;
}

uint16_t mmio_read16(uintptr_t addr) {
    uint16_t val;
    __asm__ volatile("movw (%1), %%ax" : "=a"(val) : "r"(addr) : "memory");
    return val;
}

uint32_t mmio_read32(uintptr_t addr) {
    uint32_t val;
    __asm__ volatile("movl (%1), %%eax" : "=a"(val) : "r"(addr) : "memory");
    return val;
}

void mmio_write8(uintptr_t addr, uint8_t value) {
    __asm__ volatile("movb %%al, (%1)" : : "a"(value), "r"(addr) : "memory");
}

void mmio_write16(uintptr_t addr, uint16_t value) {
    __asm__ volatile("movw %%ax, (%1)" : : "a"(value), "r"(addr) : "memory");
}

void mmio_write32(uintptr_t addr, uint32_t value) {
    __asm__ volatile("movl %%eax, (%1)" : : "a"(value), "r"(addr) : "memory");
}
