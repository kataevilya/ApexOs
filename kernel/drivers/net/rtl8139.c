#include "rtl8139.h"
#include "io.h"
#include "net.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "pic.h"
#include "pci.h"
#include "string.h"
#include "idt.h"
#include <stddef.h>

#define RTL_RXBUF_VIRT 0xFFFFFFFFF0000000ull
#define RTL_MMIO_VIRT  (RTL_RXBUF_VIRT + RTL_RXBUF_SIZE)

static uint8_t *rx_buf = NULL;
static uint32_t io_base = 0;
static uint8_t  mac[6];
static int rtl8139_bar_is_io = 0;
static uint8_t *rtl8139_mmio = NULL;

static inline uint8_t rtl_read8(uint32_t offset) {
    if (rtl8139_bar_is_io) {
        return inb(io_base + offset);
    }
    return rtl8139_mmio[offset];
}

static inline void rtl_write8(uint32_t offset, uint8_t value) {
    if (rtl8139_bar_is_io) {
        outb(io_base + offset, value);
    } else {
        rtl8139_mmio[offset] = value;
    }
}

static inline uint16_t rtl_read16(uint32_t offset) {
    if (rtl8139_bar_is_io) {
        return inw(io_base + offset);
    }
    return *(volatile uint16_t *)(rtl8139_mmio + offset);
}

static inline void rtl_write16(uint32_t offset, uint16_t value) {
    if (rtl8139_bar_is_io) {
        outw(io_base + offset, value);
    } else {
        *(volatile uint16_t *)(rtl8139_mmio + offset) = value;
    }
}

static inline uint32_t rtl_read32(uint32_t offset) {
    if (rtl8139_bar_is_io) {
        return inl(io_base + offset);
    }
    return *(volatile uint32_t *)(rtl8139_mmio + offset);
}

static inline void rtl_write32(uint32_t offset, uint32_t value) {
    if (rtl8139_bar_is_io) {
        outl(io_base + offset, value);
    } else {
        *(volatile uint32_t *)(rtl8139_mmio + offset) = value;
    }
}

void rtl8139_get_mac(uint8_t m[6]) {
    for (int i = 0; i < 6; i++) m[i] = mac[i];
}

static void rtl8139_irq_handler(struct registers *regs) {
    (void)regs;
    serial_write("[rtl8139] IRQ\n");
    uint8_t buf[2048];
    size_t len;
    while (rtl8139_poll(buf, sizeof(buf), &len)) {
        net_rx_handler(buf, len);
    }
}

void rtl8139_init(void) {
    struct pci_device dev;
    if (!pci_find_device(0x02, 0x00, 0xFF, &dev)) {
        serial_write("[rtl8139] no Ethernet controller found via PCI\n");
        return;
    }
    serial_printf("[rtl8139] found at %u:%u.%u\n", dev.bus, dev.slot, dev.func);

    uint32_t bar0_raw = dev.bar[0];
    if (bar0_raw == 0 || bar0_raw == 0xFFFFFFFF) {
        serial_write("[rtl8139] BAR0 not present\n");
        return;
    }

    rtl8139_bar_is_io = (bar0_raw & 1) != 0;
    uint32_t bar0 = bar0_raw & 0xFFFFFFFC;

    if (rtl8139_bar_is_io) {
        if (bar0 == 0 || bar0 >= 0xFFFF) {
            serial_write("[rtl8139] BAR0 invalid I/O address\n");
            return;
        }
    } else {
        if (bar0 == 0 || bar0 >= 0xFFFFFFFF) {
            serial_write("[rtl8139] BAR0 invalid memory address\n");
            return;
        }
    }

    io_base = bar0;
    serial_printf("[rtl8139] io_base=0x%x (%s)\n", io_base, rtl8139_bar_is_io ? "I/O" : "MMIO");

    if (!rtl8139_bar_is_io) {
        uint64_t phys_page = bar0 & ~(VMM_PAGE_SIZE - 1);
        uint32_t offset = bar0 & (VMM_PAGE_SIZE - 1);
        vmm_map(RTL_MMIO_VIRT, phys_page, VMM_PRESENT | VMM_WRITABLE);
        rtl8139_mmio = (uint8_t *)(RTL_MMIO_VIRT + offset);
    }

    for (int i = 0; i < 6; i++) {
        mac[i] = rtl_read8(RTL8139_MAC0 + i);
    }
    serial_printf("[rtl8139] MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    rtl_write8(RTL8139_CMD, RTL_CMD_RESET);
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

    serial_printf("[rtl8139] RX buf virt=0x%llx phys=0x%llx\n",
                  (unsigned long long)RTL_RXBUF_VIRT, (unsigned long long)phys);

    rtl_write32(RTL8139_RXBUF, (uint32_t)(phys & 0xFFFFFFFF));

    rtl_write32(0x44, 0x0F);
    rtl_write16(0x40, 0x0300);

    rtl_write16(RTL8139_ISR, 0xFFFF);
    rtl_write16(RTL8139_IMR, 0x0005);

    uint8_t cmd = rtl_read8(RTL8139_CMD);
    uint16_t imr = rtl_read16(RTL8139_IMR);
    uint16_t isr = rtl_read16(RTL8139_ISR);
    serial_printf("[rtl8139] CMD=0x%02X IMR=0x%04X ISR=0x%04X\n", cmd, imr, isr);

    rtl_write8(RTL8139_CMD, RTL_CMD_RX_ENA | RTL_CMD_TX_ENA);

    register_interrupt_handler(51, rtl8139_irq_handler);
    pic_unmask_irq(11);

    serial_write("[rtl8139] initialized OK\n");
}

void rtl8139_send(const void *data, size_t len) {
    if (io_base == 0) return;
    uint64_t phys = vmm_get_phys((uint64_t)data);
    if (phys == 0) {
        serial_write("[rtl8139] send: cannot resolve phys addr\n");
        return;
    }
    serial_printf("[rtl8139] send: phys=0x%lx len=%u\n", (unsigned long)phys, (unsigned)len);
    rtl_write32(RTL8139_TXADDR0, (uint32_t)(phys & 0xFFFFFFFF));
    uint32_t txcmd = ((uint32_t)len << 16) | 0x00;
    rtl_write32(RTL8139_TXSTAT0, txcmd);
}

int rtl8139_poll(void *buf, size_t max_len, size_t *out_len) {
    if (io_base == 0 || rx_buf == NULL) return 0;
    uint16_t isr = rtl_read16(RTL8139_ISR);
    if (isr != 0) {
        serial_printf("[rtl8139] ISR=0x%04X\n", isr);
    }
    if (!(isr & RTL_ISR_ROK)) {
        rtl_write16(RTL8139_ISR, 0xFFFF);
        return 0;
    }
    rtl_write16(RTL8139_ISR, 0xFFFF);

    uint16_t capr = rtl_read16(RTL8139_CAPR);
    uint16_t capw = rtl_read16(0x3A);
    uint32_t status = *(volatile uint32_t *)(rx_buf + capr);
    uint16_t length = (uint16_t)(status >> 16);
    if ((status & 0x03) != 0x01) {
        uint16_t next_capr = (uint16_t)((capr + 4 + length + 3) & 0xFFFC);
        rtl_write16(RTL8139_CAPR, next_capr);
        return 0;
    }
    uint8_t *packet = rx_buf + capr + 4;
    serial_printf("[rtl8139] poll: capr=0x%04X capw=0x%04X status=0x%08X len=%u\n",
                  capr, capw, status, length);
    size_t copy_len = length;
    if (copy_len > max_len) copy_len = max_len;
    for (size_t i = 0; i < copy_len; i++) {
        ((uint8_t *)buf)[i] = packet[i];
    }
    *out_len = copy_len;
    uint16_t next_capr = (uint16_t)((capr + 4 + length + 3) & 0xFFFC);
    rtl_write16(RTL8139_CAPR, next_capr);
    return 1;
}
