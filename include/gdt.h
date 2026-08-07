#ifndef APEXOS_GDT_H
#define APEXOS_GDT_H

#include <stdint.h>

/* Селекторы — фиксированный layout нашей GDT (см. gdt.c):
   0x00 null, 0x08 kernel code, 0x10 kernel data,
   0x18 user code (RPL=3 -> 0x1B), 0x20 user data (RPL=3 -> 0x23),
   0x28 TSS (16-байтный system descriptor, занимает 2 слота). */
#define GDT_SEL_KERNEL_CODE 0x08
#define GDT_SEL_KERNEL_DATA 0x10
#define GDT_SEL_USER_CODE   0x18
#define GDT_SEL_USER_DATA   0x20
#define GDT_SEL_TSS         0x28

#define GDT_SEL_USER_CODE_RPL3 (GDT_SEL_USER_CODE | 3)
#define GDT_SEL_USER_DATA_RPL3 (GDT_SEL_USER_DATA | 3)

/* Обычный 8-байтный дескриптор code/data сегмента. */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

/* System descriptor (TSS) в long mode — 16 байт: обычный 8-байтный
   дескриптор плюс старшие 32 бита base и зарезервированные 32 бита. */
struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Task State Segment в 64-битном режиме используется ядром только ради
   RSP0 — адреса стека ring0, на который CPU переключается при
   прерывании/syscall из ring3. Layout фиксирован спецификацией Intel
   (104 байта), поле IST пока не используем (все 0). */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* gdt_init() строит и загружает настоящую GDT ядра (заменяет временную
   из boot.S) и инициализирует TSS с rsp0 = текущий стек ядра. */
void gdt_init(void);

/* Обновляет RSP0 в TSS — вызывается планировщиком при переключении
   задач, чтобы ring3->ring0 переход всегда попадал на актуальный стек
   текущей задачи. Пока используется только с начальным стеком ядра. */
void tss_set_kernel_stack(uint64_t rsp0);

#endif /* APEXOS_GDT_H */
