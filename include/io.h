#ifndef APEXOS_IO_H
#define APEXOS_IO_H

#include <stdint.h>

/* Обёртки над in/out для 8/16/32-битных портов. static inline, чтобы
   не тянуть отдельный translation unit ради пары инструкций. */

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Небольшая пауза для "медленных" устройств — классический трюк:
   запись в неиспользуемый порт 0x80 занимает примерно 1 мкс на
   реальном железе. Используется вместо busy-loop без обоснования. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif /* APEXOS_IO_H */
