#ifndef APEXOS_PIC_H
#define APEXOS_PIC_H

#include <stdint.h>

/* После remap: master PIC -> векторы 0x20-0x27 (IRQ0-7),
   slave PIC -> векторы 0x28-0x2F (IRQ8-15). Обязательно делаем remap —
   иначе IRQ0-7 конфликтуют с CPU-исключениями 0x08-0x0F по умолчанию. */
#define PIC1_VECTOR_OFFSET 0x20
#define PIC2_VECTOR_OFFSET 0x28

void pic_remap(uint8_t offset1, uint8_t offset2);
void pic_send_eoi(uint8_t irq);
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);

/* Маскирует все 16 линий — вызывается сразу после remap, до того как
   у нас появятся зарегистрированные обработчики. Иначе устройство
   может засыпать IRQ ещё до того, как для него есть handler. */
void pic_mask_all(void);

#endif /* APEXOS_PIC_H */
