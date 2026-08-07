#ifndef APEXOS_USER_SYSCALL_H
#define APEXOS_USER_SYSCALL_H

/*
 * Минимальные обёртки над ApexOS syscall ABI (int 0x80, соглашение
 * ядра: rax=номер, rdi/rsi/rdx=аргументы — см. include/syscall.h в
 * исходниках ядра). НЕ libc: нет printf/malloc/файлов — если нужно
 * что-то из этого, пишите руками через эти два примитива.
 *
 * Собирается на ХОСТЕ через `make user-app SRC=...` (см. Makefile) —
 * компилятора C внутри ApexOS нет и в обозримом будущем не будет.
 */

typedef unsigned long size_t_ni;

static inline long sys_write(const char *buf, size_t_ni len) {
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(1), "D"(buf), "S"(len)
        : "memory"
    );
    return ret;
}

__attribute__((noreturn))
static inline void sys_exit(long code) {
    __asm__ volatile (
        "int $0x80"
        :
        : "a"(0), "D"(code)
        : "memory"
    );
    __builtin_unreachable(); /* SYS_EXIT не возвращается — компилятор не может это знать сам */
}

/* Блокирующее чтение ОДНОГО символа с клавиатуры. Возвращает 1 при
   успехе (символ записан в *out), -1 при ошибке (указатель вне
   разрешённого диапазона). Всегда именно 1 байт за вызов — честное
   ограничение текущего SYS_READ, не буферизованное построчное чтение. */
static inline long sys_read_char(char *out) {
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(2), "D"(out), "S"(1)
        : "memory"
    );
    return ret;
}

static inline size_t_ni ni_strlen(const char *s) {
    size_t_ni n = 0;
    while (s[n]) n++;
    return n;
}

#endif /* APEXOS_USER_SYSCALL_H */
