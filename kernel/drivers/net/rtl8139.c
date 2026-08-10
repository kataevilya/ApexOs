#include "rtl8139.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "pic.h"
#include "pci.h"
#include "string.h"
#include <stddef.h>

#define RTL_RXBUF_VIRT 0xFFFFFFFFF0000000ull

static uint8_t *rx_buf = NULL;
static uint32_t io_base = 0;
static uint8_t  mac[6];

void rtl8139_get_mac(uint8_t m[6]) {
    for (int i = 0; i < 6; i++) m[i] = mac[i];
}

void rtl8139_init(void) {
    struct pci_device dev;
    if (!pci_find_device(0x02, 0x00, 0xFF, &dev)) {
        serial_write("[rtl8139] no Ethernet controller found via PCI\n");
        return;
    }
    serial_printf("[rtl8139] found at %u:%u.%u\n", dev.bus, dev.slot, dev.func);

    uint32_t bar0 = dev.bar[0] & 0xFFFFFFFC;
    if (bar0 == 0 || bar0 >= 0xFFFF0000) {
        serial_write("[rtl8139] BAR0 invalid\n");
        return;
    }

    io_base = bar0;
    serial_printf("[rtl8139] io_base=0x%x\n", io_base);

    for (int i = 0; i < 6; i++) {
        mac[i] = inb(io_base + RTL8139_MAC0 + i);
    }
    serial_printf("[rtl8139] MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    outb(io_base + RTL8139_CMD, RTL_CMD_RESET);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile ("nop");

    uint64_t phys = 0;
    for (uint64_t f = 0; f < (RTL_RXBUF_SIZE / 4096); f++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            serial_write("[rtl8139] out of memory for rx buffer\n");
            return;
        }
        if (f == 0) phys = frame;
        vmm_map(RTL_RXBUF_VIRT + f * 4096, frame, VMM_PRESENT | VMM_WRITABLE);
    }
    rx_buf = (uint8_t *)RTL_RXBUF_VIRT;
    memset(rx_buf, 0, RTL_RXBUF_SIZE);

    outl(io_base + RTL8139_RXBUF, (uint32_t)(phys & 0xFFFFFFFF));

    outl(io_base + 0x44, 0x0F);
    outl(io_base + 0x40, 0x0300);

    outw(io_base + RTL8139_ISR, 0xFFFF);
    outw(io_base + RTL8139_IMR, 0x0005);

    outb(io_base + RTL8139_CMD, RTL_CMD_RX_ENA | RTL_CMD_TX_ENA);

    serial_write("[rtl8139] initialized OK\n");
}

void rtl8139_send(const void *data, size_t len) {
    if (io_base == 0) return;
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    for (i = 0; i < len; i++) {
        outb(io_base + RTL8139_TXADDR0, p[i]);
    }
    uint16_t status = (uint16_t)((len << 16) | 0x00);
    outl(io_base + RTL8139_TXSTAT0, status);
}

int rtl8139_poll(void *buf, size_t max_len, size_t *out_len) {
    if (io_base == 0 || rx_buf == NULL) return 0;
    uint16_t isr = inw(io_base + RTL8139_ISR);
    if (!(isr & RTL_ISR_ROK)) {
        outw(io_base + RTL8139_ISR, RTL_ISR_ROK);
        return 0;
    }
    outw(io_base + RTL8139_ISR, RTL_ISR_ROK);

    uint16_t capr = inw(io_base + RTL8139_CAPR);
    uint32_t *rx_ptr = (uint32_t *)(rx_buf + (capr + 0x10) % RTL_RXBUF_SIZE);
    uint32_t status = rx_ptr[0];
    uint32_t length = rx_ptr[1];
    if ((status & 0x03) != 0x01) {
        outw(io_base + RTL8139_CAPR, (uint16_t)((capr + length + 4 + 3) & 0xFFFC));
        return 0;
    }
    uint8_t *packet = rx_buf + (capr + 4) % RTL_RXBUF_SIZE;
    size_t copy_len = length;
    if (copy_len > max_len) copy_len = max_len;
    for (size_t i = 0; i < copy_len; i++) {
        ((uint8_t *)buf)[i] = packet[i];
    }
    *out_len = copy_len;
    outw(io_base + RTL8139_CAPR, (uint16_t)((capr + length + 4 + 3) & 0xFFFC));
    return 1;
}
