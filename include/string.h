#ifndef APEXOS_STRING_H
#define APEXOS_STRING_H

#include <stddef.h>

/* Собственные реализации — ядро freestanding и libc не подключает.
   Все функции безопасны относительно size_t: не полагаются на то,
   что n никогда не будет 0 или огромным. */

void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memset(void *dest, int value, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

/* Классический strcpy — без проверки границ, как в libc. Оставлен по
   требованиям задания, но внутри ядра НЕ использовать напрямую для
   данных, длина которых не проверена заранее (user input, диск, сеть).
   Для них — strcpy_safe ниже. */
char *strcpy(char *restrict dest, const char *restrict src);

/* Безопасная версия: dest_size обязателен и никогда не превышается.
   Возвращает 0 при успехе, -1 если src обрезан (не поместился) —
   в этом случае dest всё равно корректно NUL-терминирован. */
int strcpy_safe(char *dest, size_t dest_size, const char *src);

#endif /* APEXOS_STRING_H */
