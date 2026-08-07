#ifndef APEXOS_SYSCALL_H
#define APEXOS_SYSCALL_H

#include "idt.h"

/* Минимальный набор — ровно то, что нужно тестовой userspace-программе.
   Дальше по мере появления VFS/scheduler будет расширяться (open/read/
   write на реальные fd, fork/exec и т.д.) — не выдумываю остальные
   номера заранее, чтобы не фиксировать ABI, который потом придётся
   ломать. */
#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_READ  2

/* syscall_init(): регистрирует вектор 0x80 как доступный из ring3
   (DPL=3 в дескрипторе IDT — иначе CPU сгенерирует #GP при попытке
   userspace-кода вызвать int 0x80). */
void syscall_init(void);

/* syscall_dispatch: вызывается из isr_handler для vector==0x80.
   Соглашение о регистрах (наше собственное, не Linux ABI):
   rax=номер syscall, rdi/rsi/rdx=аргументы. Результат записывается
   обратно в regs->rax перед iretq. */
void syscall_dispatch(struct registers *regs);

#endif /* APEXOS_SYSCALL_H */
