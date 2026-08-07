#ifndef APEXOS_PCI_H
#define APEXOS_PCI_H

#include <stdint.h>

struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, revision;
    uint32_t bar[6];
};

/* Читает/пишет 32-битный регистр конфигурационного пространства PCI
   через legacy-порты 0xCF8/0xCFC (механизм #1) — работает на любом
   x86 с PCI, не требует MMCONFIG/ACPI. offset должен быть выровнен
   на 4 байта. */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

typedef void (*pci_enum_cb)(const struct pci_device *dev, void *ctx);

/* pci_enumerate: перебирает шины 0-255, слоты 0-31, функции 0-7 (только
   если устройство multi-function — бит 7 байта header_type), вызывает
   callback для каждого реально существующего устройства (vendor_id !=
   0xFFFF). Только ЧТЕНИЕ конфигурационного пространства — ничего не
   меняет на устройствах. */
void pci_enumerate(pci_enum_cb callback, void *ctx);

/* Ищет первое устройство с данными class_code/subclass/prog_if (0xFF в
   любом поле = "не важно, любое значение"). Возвращает 1 и заполняет
   *out, если нашлось, иначе 0. */
int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, struct pci_device *out);

#endif /* APEXOS_PCI_H */
