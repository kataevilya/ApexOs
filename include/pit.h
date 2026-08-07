#ifndef APEXOS_PIT_H
#define APEXOS_PIT_H

#include <stdint.h>

/* pit_init: программирует канал 0 PIT на заданную частоту (Гц) и
   регистрирует обработчик IRQ0 (вектор 32 после remap PIC). Не
   размаскирует IRQ0 сама — это делает вызывающий код после того, как
   убедится, что IDT/PIC уже готовы. */
void pit_init(uint32_t frequency_hz);

/* Количество тиков таймера с момента pit_init(). Инкрементируется в
   обработчике IRQ0, вызываемом из isr_handler. */
uint64_t pit_get_ticks(void);

#endif /* APEXOS_PIT_H */
