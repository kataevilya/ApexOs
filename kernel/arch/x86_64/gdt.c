#include "gdt.h"
#include "serial.h"
#include <stddef.h>

/* Реализованы в gdt_flush.S: перезагружают CS/DS/ES/FS/GS/SS новыми
   селекторами (через far-return трюк, единственный способ сменить CS
   в long mode) и TR соответственно. */
extern void gdt_flush(uint64_t gdt_pointer_addr);
extern void tss_flush(void);

extern uint8_t kernel_stack_top[]; /* entry.S — начальный стек ядра */

/* Вся GDT — один packed struct, чтобы гарантировать точный layout байт
   без паддинга компилятора между разнотипными дескрипторами. */
struct gdt_table {
    struct gdt_entry    null;
    struct gdt_entry    kernel_code;
    struct gdt_entry    kernel_data;
    struct gdt_entry    user_code;
    struct gdt_entry    user_data;
    struct tss_descriptor tss;
} __attribute__((packed));

static struct gdt_table g_gdt;
static struct gdt_pointer g_gdt_pointer;
static struct tss g_tss;

static void encode_entry(struct gdt_entry *entry, uint8_t access, uint8_t granularity) {
    /* Все дескрипторы code/data у нас flat (base=0, limit=0xFFFFF) —
       в 64-битном режиме CPU всё равно игнорирует base/limit для
       обычных code/data сегментов (кроме FS/GS base, которые задаются
       отдельными MSR, не через GDT). Явно занулаем/заполняем, а не
       оставляем как есть, чтобы не зависеть от начального состояния
       статической памяти. */
    entry->limit_low   = 0xFFFF;
    entry->base_low     = 0;
    entry->base_mid     = 0;
    entry->access        = access;
    entry->granularity  = granularity;
    entry->base_high    = 0;
}

static void encode_tss_descriptor(struct tss_descriptor *desc, uint64_t base, uint32_t limit) {
    desc->limit_low  = (uint16_t)(limit & 0xFFFF);
    desc->base_low    = (uint16_t)(base & 0xFFFF);
    desc->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    desc->access       = 0x89; /* present=1, DPL=0, type=1001 (64-bit TSS, available) */
    desc->granularity = (uint8_t)((limit >> 16) & 0x0F); /* G=0: limit в байтах, TSS мал */
    desc->base_high   = (uint8_t)((base >> 24) & 0xFF);
    desc->base_upper  = (uint32_t)(base >> 32);
    desc->reserved     = 0;
}

void gdt_init(void) {
    /* access byte: [P=1][DPL 2 bit][S=1][Type 4 bit]
       code: type=1010 (exec/read), data: type=0010 (read/write) */
    encode_entry(&g_gdt.null, 0x00, 0x00);
    encode_entry(&g_gdt.kernel_code, 0x9A, 0xAF); /* P,DPL0,S,exec/read; G=1,L=1 */
    encode_entry(&g_gdt.kernel_data, 0x92, 0xAF); /* P,DPL0,S,read/write */
    encode_entry(&g_gdt.user_code,   0xFA, 0xAF); /* P,DPL3,S,exec/read */
    encode_entry(&g_gdt.user_data,   0xF2, 0xAF); /* P,DPL3,S,read/write */

    /* TSS: rsp0 указывает на текущий (пока единственный) стек ядра —
       честно, а не заглушка: реальный ring3->ring0 переход сегодня же
       использовал бы именно этот адрес, если бы он произошёл. */
    for (size_t i = 0; i < sizeof(g_tss); i++) {
        ((uint8_t *)&g_tss)[i] = 0;
    }
    g_tss.rsp0 = (uint64_t)(uintptr_t)kernel_stack_top;
    g_tss.iomap_base = sizeof(struct tss); /* нет I/O permission bitmap */

    encode_tss_descriptor(&g_gdt.tss, (uint64_t)(uintptr_t)&g_tss, sizeof(g_tss) - 1);

    g_gdt_pointer.limit = sizeof(g_gdt) - 1;
    g_gdt_pointer.base  = (uint64_t)(uintptr_t)&g_gdt;

    gdt_flush((uint64_t)(uintptr_t)&g_gdt_pointer);
    tss_flush();

    serial_printf("[gdt] loaded: kernel_code=0x%x kernel_data=0x%x user_code=0x%x user_data=0x%x tss=0x%x\n",
                  GDT_SEL_KERNEL_CODE, GDT_SEL_KERNEL_DATA,
                  GDT_SEL_USER_CODE, GDT_SEL_USER_DATA, GDT_SEL_TSS);
    serial_printf("[gdt] tss.rsp0 = 0x%lx\n", (unsigned long)g_tss.rsp0);
}

void tss_set_kernel_stack(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}
