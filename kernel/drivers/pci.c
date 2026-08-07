#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t make_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)(slot & 0x1F) << 11) |
           ((uint32_t)(func & 0x7) << 8) | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, make_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, make_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_enumerate(pci_enum_cb callback, void *ctx) {
    /* uint32_t для счётчиков намеренно, не uint8_t — bus идёт до 255
       включительно, и uint8_t-счётчик "bus<256" зациклился бы навечно
       (255+1 переполняется обратно в 0). */
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) {
                continue; /* нет устройства в этом слоте */
            }

            uint32_t header_word = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            uint8_t header_type = (uint8_t)((header_word >> 16) & 0xFF);
            int num_funcs = (header_type & 0x80) ? 8 : 1;

            for (int func = 0; func < num_funcs; func++) {
                uint32_t fid = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                if ((fid & 0xFFFF) == 0xFFFF) {
                    continue;
                }

                struct pci_device dev;
                dev.bus = (uint8_t)bus;
                dev.slot = (uint8_t)slot;
                dev.func = (uint8_t)func;
                dev.vendor_id = (uint16_t)(fid & 0xFFFF);
                dev.device_id = (uint16_t)((fid >> 16) & 0xFFFF);

                uint32_t classword = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
                dev.revision = (uint8_t)(classword & 0xFF);
                dev.prog_if = (uint8_t)((classword >> 8) & 0xFF);
                dev.subclass = (uint8_t)((classword >> 16) & 0xFF);
                dev.class_code = (uint8_t)((classword >> 24) & 0xFF);

                for (int b = 0; b < 6; b++) {
                    dev.bar[b] = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func,
                                                    (uint8_t)(0x10 + b * 4));
                }

                callback(&dev, ctx);
            }
        }
    }
}

struct find_ctx {
    uint8_t class_code, subclass, prog_if;
    struct pci_device *out;
    int found;
};

static void find_cb(const struct pci_device *dev, void *ctx_) {
    struct find_ctx *ctx = (struct find_ctx *)ctx_;
    if (ctx->found) {
        return;
    }
    if ((ctx->class_code == 0xFF || dev->class_code == ctx->class_code) &&
        (ctx->subclass == 0xFF || dev->subclass == ctx->subclass) &&
        (ctx->prog_if == 0xFF || dev->prog_if == ctx->prog_if)) {
        *ctx->out = *dev;
        ctx->found = 1;
    }
}

int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, struct pci_device *out) {
    struct find_ctx ctx = { class_code, subclass, prog_if, out, 0 };
    pci_enumerate(find_cb, &ctx);
    return ctx.found;
}
