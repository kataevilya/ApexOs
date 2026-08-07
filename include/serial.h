#ifndef APEXOS_SERIAL_H
#define APEXOS_SERIAL_H

#include <stdint.h>
#include <stddef.h>

/* COM1, стандартный адрес на PC-совместимом железе и в QEMU (-serial stdio). */
#define SERIAL_COM1_PORT 0x3F8

/* Инициализирует COM1: 38400 baud, 8N1, FIFO. Возвращает 0 при успехе,
   -1 если loopback-тест показал, что порт не отвечает (например, под
   железом без физического UART) — тогда весь остальной serial-вывод
   молча no-op, а не зависает. */
int serial_init(void);

void serial_putc(char c);
void serial_write(const char *str);
void serial_write_len(const char *buf, size_t len);

/* Кольцевой лог-буфер: КАЖДЫЙ байт, когда-либо прошедший через
   serial_putc (то есть весь существующий serial_write/serial_printf
   вывод по всему ядру, без переделки сотен уже написанных вызовов),
   автоматически копируется сюда. `dmesg` в shell читает это через
   serial_log_copy() — честный, содержательный лог "из коробки", а не
   отдельная система логирования, которую пришлось бы заново
   протаскивать через весь код. */
size_t serial_log_available(void); /* сколько байт реально есть (<= размера кольца) */
void serial_log_copy(char *out, size_t max_len); /* копирует последние max_len байт (или меньше) в хронологическом порядке */

/* Простейший форматированный вывод для раннего лога: поддерживает
   %s %d %u %x %p %c %%. Никакого динамического выделения памяти. */
void serial_printf(const char *fmt, ...);

/* va_list-версия — нужна коду вроде panic(), который сам принимает
   variadic-аргументы и должен передать их дальше, не разворачивая. */
#include <stdarg.h>
void serial_vprintf(const char *fmt, va_list args);

#endif /* APEXOS_SERIAL_H */
