#ifndef APEXOS_TASK_H
#define APEXOS_TASK_H

/*
 * Планировщик задач ядра — округ-робин, вытесняющий (переключение по
 * таймеру, PIT @ 100 Гц) ИЛИ добровольный (task_yield()). Использует
 * тот же k_setjmp/k_longjmp, что и process_run() (см. context.S).
 *
 * ЧЕСТНЫЕ ограничения:
 *   - только kernel-mode потоки (ring0), НЕ отдельные ring3-процессы —
 *     для тех нужны отдельные адресные пространства (per-process page
 *     tables), которых у нас пока нет;
 *   - пока активна ring3-программа через process_run() (`run`/Ctrl+R),
 *     планировщик НЕ переключает задачи — она не представлена слотом
 *     здесь и выполняется до собственного SYS_EXIT безраздельно;
 *   - нет приоритетов, нет sleep()/таймеров — чистый round-robin по
 *     готовым (READY) задачам;
 *   - максимум MAX_TASKS задач одновременно, каждая со своим стеком
 *     фиксированного размера, который не освобождается после
 *     завершения задачи (честная, простая, но не идеальная политика).
 */

int task_create(const char *name, void (*entry)(void));
void task_yield(void);
void scheduler_tick(void);
void scheduler_set_preemption(int enabled);
int scheduler_preemption_enabled(void);
int scheduler_task_count(void);
const char *scheduler_task_name(int index);
int scheduler_task_is_done(int index);

#endif /* APEXOS_TASK_H */
