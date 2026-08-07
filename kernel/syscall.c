#include "syscall.h"
#include "serial.h"
#include "console.h"
#include "process.h"
#include "keyboard.h"

/* Граница "это точно адрес ядра, не отдавай userspace-указателю" —
   пока не заменена настоящей проверкой "действительно замаплено этому
   процессу" (нужны per-process page tables, которых ещё нет). Честное
   ограничение этого этапа, а не забытая проверка. */
#define KERNEL_ADDRESS_SPACE_START 0xFFFF800000000000ull

void syscall_init(void) {
    idt_set_user_callable(0x80);
    serial_write("[syscall] int 0x80 enabled for ring3 (SYS_EXIT=0, SYS_WRITE=1, SYS_READ=2)\n");
}

void syscall_dispatch(struct registers *regs) {
    uint64_t num = regs->rax;

    switch (num) {
        case SYS_READ: {
            char *buf = (char *)(uintptr_t)regs->rdi;
            uint64_t len = regs->rsi;

            if ((uint64_t)(uintptr_t)buf >= KERNEL_ADDRESS_SPACE_START) {
                serial_write("[syscall] SYS_READ: rejected pointer in kernel address space\n");
                regs->rax = (uint64_t)-1;
                return;
            }
            if (len == 0) {
                regs->rax = 0;
                return;
            }

            /* Мы сейчас исполняемся ВНУТРИ обработчика int 0x80 —
               interrupt gate уже сбросил IF на входе. Если ждать
               символ через hlt с IF=0, ни один IRQ (в т.ч. сама
               клавиатура на IRQ1) не сможет разбудить CPU — вечный
               залип. Поэтому явно включаем прерывания на время
               ожидания; iretq в конце isr_common_stub всё равно
               восстановит IF из исходных RFLAGS ring3-кода (там оно
               и так было 1), так что это не меняет итоговое состояние. */
            __asm__ volatile ("sti");
            char c;
            while (!keyboard_read_char(&c)) {
                /* Намеренно без hlt: мы вложены в обработчик прерывания,
                   а не в обычный поток ядра — просто активно ждём. */
            }
            buf[0] = c;
            regs->rax = 1;
            return;
        }

        case SYS_WRITE: {
            const char *buf = (const char *)(uintptr_t)regs->rdi;
            uint64_t len = regs->rsi;

            if ((uint64_t)(uintptr_t)buf >= KERNEL_ADDRESS_SPACE_START) {
                serial_write("[syscall] SYS_WRITE: rejected pointer in kernel address space\n");
                regs->rax = (uint64_t)-1;
                return;
            }
            if (len > 4096) {
                len = 4096; /* разумный предел для теста — не безлимитная запись по указателю */
            }

            serial_write_len(buf, len);
            console_write_len(buf, len);
            regs->rax = len;
            return;
        }

        case SYS_EXIT: {
            uint64_t code = regs->rdi;
            serial_printf("[syscall] SYS_EXIT: user program exited with code %lu\n",
                          (unsigned long)code);
            /* Раньше здесь был halt_forever() — вся система вешалась
               после ЛЮБОЙ запущенной программы. process_exit_to_kernel
               возвращает управление туда, откуда был вызван
               process_run() (shell/редактор), через ручной context
               switch — не полноценный планировщик, но хотя бы не
               билет в один конец. */
            process_exit_to_kernel((int)code);
            /* недостижимо: process_exit_to_kernel не возвращается */
        }

        default:
            serial_printf("[syscall] unknown syscall number %lu\n", (unsigned long)num);
            regs->rax = (uint64_t)-1;
            return;
    }
}
