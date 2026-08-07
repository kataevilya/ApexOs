#include "pit.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include "task.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND        0x43
#define PIT_BASE_FREQUENCY 1193182u /* Гц, частота встроенного генератора PIT */

/* volatile: изменяется из обработчика прерывания, читается из обычного
   кода (busy-wait в main). Без volatile компилятор вправе закэшировать
   значение в регистре и никогда не увидеть обновление. */
static volatile uint64_t g_pit_ticks = 0;

static void pit_irq_handler(struct registers *regs) {
    (void)regs; /* таймеру не нужен контекст прерывания */
    g_pit_ticks++;
    scheduler_tick(); /* может переключить задачу и не "вернуться" сюда обычным
                          образом — безопасно, т.к. EOI уже отправлен isr_handler
                          ДО вызова этого обработчика (см. idt.c) */
}

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 100; /* разумный дефолт вместо деления на 0 */
    }

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    /* PIT-делитель — 16-битный счётчик: 0 в регистре означает 65536,
       но 0 как результат деления (frequency_hz > BASE_FREQUENCY) для
       нас является ошибкой конфигурации, а не "самой быстрой частотой" —
       поэтому явно клэмпим в допустимый диапазон [1, 0xFFFF]. */
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    /* 0x36 = channel 0, access mode lobyte/hibyte, mode 3 (square wave) */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    register_interrupt_handler(32, pit_irq_handler); /* IRQ0 -> vector 32 */

    uint32_t actual_hz = PIT_BASE_FREQUENCY / divisor;
    serial_printf("[pit] channel 0 programmed: divisor=%u (~%u Hz)\n", divisor, actual_hz);
}

uint64_t pit_get_ticks(void) {
    return g_pit_ticks;
}
