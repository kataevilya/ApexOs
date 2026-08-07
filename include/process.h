#ifndef APEXOS_PROCESS_H
#define APEXOS_PROCESS_H

#include <stdint.h>

/*
 * process_run: загружает управление в ring3 (через enter_usermode) и
 * ВОЗВРАЩАЕТСЯ, когда программа вызовет SYS_EXIT — это не полноценная
 * многозадачность (нет вытесняющего планировщика, нет одновременно
 * работающих процессов), а ручной context switch (см. context.S):
 * мы запоминаем точку возврата перед входом в ring3 и восстанавливаем
 * её из обработчика syscall'а. Пока программа выполняется, ядро
 * полностью останавливается (ничего не может прерваться и продолжить
 * что-то ещё) — честное ограничение, но `run`/Ctrl+R хотя бы отдают
 * управление обратно в shell, а не вешают систему навсегда.
 *
 * Возвращает код завершения программы (то, что она передала в
 * SYS_EXIT через rdi).
 */
int process_run(uint64_t entry, uint64_t user_stack_top);

/* Нужна планировщику (kernel/task.c): пока ring3-программа выполняется
   через process_run(), таймер НЕ должен пытаться переключить на неё
   как на "задачу" — она не представлена слотом в task.c, у неё свой
   отдельный механизм возврата через k_longjmp в process_exit_to_kernel. */
int process_is_active(void);

/* process_exit_to_kernel: вызывается из syscall.c при SYS_EXIT, не
   возвращается — либо k_longjmp обратно в process_run(), либо (вне
   этого контекста, не должно происходить) halt_forever(). */
__attribute__((noreturn))
void process_exit_to_kernel(int code);

#endif /* APEXOS_PROCESS_H */
