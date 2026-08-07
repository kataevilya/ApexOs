#include "serial.h"
#include "io.h"
#include <stdarg.h>
#include "fmt.h"

/* UART 16550 register offsets относительно SERIAL_COM1_PORT */
#define REG_DATA          0
#define REG_INT_ENABLE    1
#define REG_FIFO_CTRL     2
#define REG_LINE_CTRL     3
#define REG_MODEM_CTRL    4
#define REG_LINE_STATUS   5

#define LSR_TX_EMPTY   (1 << 5)

/* Порт считается неисправным/отсутствующим, если инициализация
   провалилась (loopback test). Тогда все операции — no-op, а не hang. */
static int serial_ready = 0;

/* Любое ожидание готовности "железа" должно иметь верхнюю границу
   итераций — иначе сломанный/отсутствующий UART повесит всё ядро. */
#define SERIAL_WAIT_TIMEOUT 100000

/* Кольцевой лог-буфер — независим от того, есть ли реальный UART.
   Пишется на КАЖДЫЙ вызов serial_putc, поэтому весь уже существующий
   по всему ядру serial_write/serial_printf вывод автоматически
   доступен через dmesg в shell, без переделки сотен вызовов. */
#define LOG_RING_SIZE (32u * 1024u)
static char g_log_ring[LOG_RING_SIZE];
static uint32_t g_log_head = 0;
static uint32_t g_log_total_written = 0;

static void log_ring_push(char c) {
    g_log_ring[g_log_head] = c;
    g_log_head = (g_log_head + 1) % LOG_RING_SIZE;
    g_log_total_written++;
}

size_t serial_log_available(void) {
    return (g_log_total_written < LOG_RING_SIZE) ? g_log_total_written : LOG_RING_SIZE;
}

void serial_log_copy(char *out, size_t max_len) {
    size_t avail = serial_log_available();
    size_t to_copy = (avail < max_len) ? avail : max_len;
    /* Последние to_copy байт заканчиваются прямо перед g_log_head
       (текущей позицией записи). +LOG_RING_SIZE перед % — чтобы не
       уйти в отрицательные значения при беззнаковой арифметике. */
    size_t copy_start = ((size_t)g_log_head + LOG_RING_SIZE - to_copy) % LOG_RING_SIZE;
    for (size_t i = 0; i < to_copy; i++) {
        out[i] = g_log_ring[(copy_start + i) % LOG_RING_SIZE];
    }
}

int serial_init(void) {
    outb(SERIAL_COM1_PORT + REG_INT_ENABLE, 0x00); /* отключаем прерывания порта */
    outb(SERIAL_COM1_PORT + REG_LINE_CTRL, 0x80);   /* DLAB=1, чтобы задать делитель */
    outb(SERIAL_COM1_PORT + 0, 0x03);               /* делитель = 3 -> 38400 baud (lo) */
    outb(SERIAL_COM1_PORT + 1, 0x00);               /* делитель (hi) */
    outb(SERIAL_COM1_PORT + REG_LINE_CTRL, 0x03);   /* 8 бит, без чётности, 1 стоп-бит, DLAB=0 */
    outb(SERIAL_COM1_PORT + REG_FIFO_CTRL, 0xC7);   /* включить FIFO, очистить, порог 14 байт */
    outb(SERIAL_COM1_PORT + REG_MODEM_CTRL, 0x0B);  /* IRQ enable, RTS/DSR set */

    /* Loopback self-test: временно включаем loopback (бит 4 modem control),
       посылаем тестовый байт и проверяем, что он же вернулся. Так отличаем
       реально существующий UART от отсутствующего порта. */
    outb(SERIAL_COM1_PORT + REG_MODEM_CTRL, 0x1E);
    outb(SERIAL_COM1_PORT + REG_DATA, 0xAE);
    if (inb(SERIAL_COM1_PORT + REG_DATA) != 0xAE) {
        serial_ready = 0;
        return -1;
    }

    /* Возвращаем нормальный (не loopback) режим работы */
    outb(SERIAL_COM1_PORT + REG_MODEM_CTRL, 0x0F);
    serial_ready = 1;
    return 0;
}

static int wait_for_tx_empty(void) {
    for (int i = 0; i < SERIAL_WAIT_TIMEOUT; i++) {
        if (inb(SERIAL_COM1_PORT + REG_LINE_STATUS) & LSR_TX_EMPTY) {
            return 0;
        }
    }
    return -1; /* timeout — порт завис или отвалился; не блокируем ядро навсегда */
}

void serial_putc(char c) {
    log_ring_push(c); /* лог пишем ВСЕГДА, даже если UART не найден или ещё не готов */
    if (!serial_ready) {
        return;
    }
    if (c == '\n') {
        serial_putc('\r');
    }
    if (wait_for_tx_empty() != 0) {
        serial_ready = 0; /* порт перестал отвечать — дальше молчим */
        return;
    }
    outb(SERIAL_COM1_PORT + REG_DATA, (uint8_t)c);
}

void serial_write(const char *str) {
    while (*str) {
        serial_putc(*str++);
    }
}

void serial_write_len(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        serial_putc(buf[i]);
    }
}

static void serial_putc_cb(void *ctx, char c) {
    (void)ctx;
    serial_putc(c);
}

void serial_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfmt(serial_putc_cb, NULL, fmt, args);
    va_end(args);
}

void serial_vprintf(const char *fmt, va_list args) {
    vfmt(serial_putc_cb, NULL, fmt, args);
}
