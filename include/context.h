#ifndef APEXOS_CONTEXT_H
#define APEXOS_CONTEXT_H

#include <stdint.h>

/*
 * struct k_jmpbuf — раскладка ДОЛЖНА точно совпадать с тем, что
 * записывает/читает k_setjmp/k_longjmp в kernel/arch/x86_64/context.S
 * (смещения 0,8,16,24,32,40,48,56 байт). Раньше это было продублировано
 * в process.c; вынесено сюда, чтобы process.c и task.c (планировщик)
 * не могли незаметно разойтись в определении структуры, от которой
 * зависит бинарная совместимость с ассемблерным кодом.
 */
struct k_jmpbuf {
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp;
    uint64_t rip;
};

/* int k_setjmp(struct k_jmpbuf *buf) — возвращает 0 при обычном вызове,
   переданное в k_longjmp значение — когда управление приходит через неё. */
extern int k_setjmp(struct k_jmpbuf *buf);

/* void k_longjmp(struct k_jmpbuf *buf, int retval) — не возвращается. */
__attribute__((noreturn)) extern void k_longjmp(struct k_jmpbuf *buf, int retval);

#endif /* APEXOS_CONTEXT_H */
