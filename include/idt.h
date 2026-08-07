#ifndef APEXOS_IDT_H
#define APEXOS_IDT_H

#include <stdint.h>

/*
 * struct registers — ДОЛЖНА побайтово соответствовать порядку push в
 * isr_common_stub (isr_stubs.S). Если поменяется порядок push там,
 * обязательно поменять и здесь, иначе C-код будет читать чужие поля.
 * Порядок (от начала структуры, т.е. от вершины стека на входе в
 * isr_handler): r15..r8, rbp, rdi, rsi, rdx, rcx, rbx, rax (push rax
 * первым -> лежит глубже всех), затем vector, error_code (pushed by
 * stub), затем rip/cs/rflags/user_rsp/ss (pushed автоматически CPU).
 */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;
    uint64_t ss;
} __attribute__((packed));

/* IDT gate descriptor — 16 байт в long mode. */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;          /* биты 0-2: индекс IST в TSS, 0 = не используется */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

typedef void (*interrupt_handler_t)(struct registers *regs);

/* idt_init(): заполняет все 256 векторов (обработчики из isr_stub_table,
   сгенерированного в isr_stubs.S) и загружает IDT через lidt. */
void idt_init(void);

/* Регистрирует C-обработчик для конкретного вектора (в первую очередь —
   для IRQ 32-47 после remap PIC). Возвращает -1, если vector вне
   диапазона [0,255], иначе 0. */
int register_interrupt_handler(uint8_t vector, interrupt_handler_t handler);

/* Поднимает DPL данного вектора до 3, разрешая ring3-коду вызывать
   его через `int N` напрямую (нужно для syscall-вектора). */
void idt_set_user_callable(uint8_t vector);

/* isr_handler — единая точка входа из ассемблерного isr_common_stub.
   Не предназначена для прямого вызова из остального C-кода. */
void isr_handler(struct registers *regs);

#endif /* APEXOS_IDT_H */
