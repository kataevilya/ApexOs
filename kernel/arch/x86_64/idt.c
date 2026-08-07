#include "idt.h"
#include "gdt.h"
#include "serial.h"
#include "panic.h"
#include "syscall.h"
#include <stddef.h>

extern void *isr_stub_table[256]; /* isr_stubs.S */

static struct idt_entry g_idt[256];
static struct idt_pointer g_idt_pointer;
static interrupt_handler_t g_handlers[256];

/* Названия исключений CPU (векторы 0-31) — только для диагностики в
   логе/panic(), не влияют на поведение. Индексы 15,21-27,31
   зарезервированы Intel и в норме никогда не срабатывают. */
static const char *const exception_names[32] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point Exception", "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication Exception", "Security Exception", "Reserved",
};

static void idt_set_gate(uint8_t vector, void *handler, uint16_t selector, uint8_t type_attr) {
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    struct idt_entry *e = &g_idt[vector];

    e->offset_low  = (uint16_t)(addr & 0xFFFF);
    e->selector     = selector;
    e->ist           = 0; /* не используем Interrupt Stack Table пока */
    e->type_attr    = type_attr;
    e->offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    e->offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    e->reserved      = 0;
}

void idt_init(void) {
    /* type_attr = 0x8E: present=1, DPL=0, type=1110 (64-bit interrupt gate).
       Interrupt gate (не trap gate) — CPU автоматически сбрасывает IF
       на входе, поэтому обработчики по умолчанию не реентерабельны
       сами в себя без явного sti внутри C-кода (мы этого пока не делаем). */
    for (int v = 0; v < 256; v++) {
        idt_set_gate((uint8_t)v, isr_stub_table[v], GDT_SEL_KERNEL_CODE, 0x8E);
        g_handlers[v] = NULL;
    }

    g_idt_pointer.limit = sizeof(g_idt) - 1;
    g_idt_pointer.base  = (uint64_t)(uintptr_t)&g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idt_pointer));

    serial_write("[idt] 256 vectors installed, IDT loaded\n");
}

int register_interrupt_handler(uint8_t vector, interrupt_handler_t handler) {
    g_handlers[vector] = handler;
    return 0;
}

/* idt_set_user_callable: поднимает DPL записи с 0 до 3 — иначе CPU
   выдаёт #GP при попытке ring3-кода выполнить `int N` на этот вектор
   (по умолчанию все 256 gate'ов DPL=0, только ядро может их вызвать
   программно). Используется ровно один раз, для вектора syscall'ов. */
void idt_set_user_callable(uint8_t vector) {
    g_idt[vector].type_attr |= 0x60; /* поднимает биты DPL (5:6) до 11 = DPL3 */
}

/* forward-декларация — определена в pic.c; здесь используем только
   для отправки EOI после обработки аппаратного IRQ. */
extern void pic_send_eoi(uint8_t irq);

void isr_handler(struct registers *regs) {
    uint64_t vector = regs->vector;

    if (vector < 32) {
        /* Breakpoint (int3) — единственное исключение, которое мы
           намеренно не считаем фатальным: используется как self-test
           полного цикла ISR (int3 -> обработчик -> iretq -> продолжение
           выполнения следующей инструкции). */
        if (vector == 3) {
            serial_printf("[isr] breakpoint (int3) at rip=0x%lx — continuing\n",
                          (unsigned long)regs->rip);
            return;
        }

        panic("CPU exception #%lu (%s) error_code=0x%lx rip=0x%lx cs=0x%lx rflags=0x%lx",
              (unsigned long)vector, exception_names[vector],
              (unsigned long)regs->error_code, (unsigned long)regs->rip,
              (unsigned long)regs->cs, (unsigned long)regs->rflags);
        return; /* недостижимо: panic() -> halt_forever() не возвращается */
    }

    if (vector < 48) {
        uint8_t irq = (uint8_t)(vector - 32);
        /* EOI отправляется ДО вызова обработчика, а не после. Раньше
           было наоборот — но обработчик PIT теперь (см. task.c) может
           переключить задачу через k_longjmp и никогда не "вернуться"
           в этот стек-фрейм обычным образом, из-за чего EOI после
           handler(regs) попросту не выполнился бы, и PIC перестал бы
           слать IRQ0 (и всё, что приоритетнее него) навсегда. Слать
           EOI заранее безопасно для 8259: он лишь означает "разрешаю
           присылать больше прерываний этой линии", что корректно даже
           если конкретно ЭТОТ обработчик ещё не "закончил" в обычном
           понимании. */
        pic_send_eoi(irq);
        interrupt_handler_t handler = g_handlers[vector];
        if (handler != NULL) {
            handler(regs);
        }
        return;
    }

    if (vector == 0x80) {
        syscall_dispatch(regs);
        return;
    }

    /* Прочие векторы 48-255 (кроме 0x80) пока ни для чего не используются
       (будущие IPI и т.д.) — просто логируем, не паникуем, т.к. это не
       аппаратная ошибка, а неожиданное, но не разрушительное событие. */
    serial_printf("[isr] unhandled interrupt vector %lu\n", (unsigned long)vector);
}
