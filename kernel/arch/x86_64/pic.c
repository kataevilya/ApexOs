#include "pic.h"
#include "io.h"
#include "serial.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_ICW4    0x01  /* ICW4 будет передан */
#define ICW1_INIT    0x10  /* обязательный бит инициализации */
#define ICW4_8086    0x01  /* 8086/88 (не MCS-80/85) mode */

#define PIC_EOI      0x20

void pic_remap(uint8_t offset1, uint8_t offset2) {
    /* Сохраняем текущие маски — restore после remap, чтобы не
       разбудить IRQ, которые вызывающий код ещё не готов обрабатывать. */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, offset1); /* ICW2: базовый вектор master */
    io_wait();
    outb(PIC2_DATA, offset2); /* ICW2: базовый вектор slave */
    io_wait();

    outb(PIC1_DATA, 4);       /* ICW3: у master'а slave висит на IRQ2 (бит 2) */
    io_wait();
    outb(PIC2_DATA, 2);       /* ICW3: slave сообщает свой cascade identity (2) */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);

    serial_printf("[pic] remapped: master=0x%x slave=0x%x\n", offset1, offset2);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_mask_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : (uint8_t)(irq - 8);
    uint8_t value = inb(port) | (uint8_t)(1u << bit);
    outb(port, value);
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : (uint8_t)(irq - 8);
    uint8_t value = inb(port) & (uint8_t)~(1u << bit);
    outb(port, value);
}

void pic_mask_all(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
