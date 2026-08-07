#ifndef APEXOS_BLOCKDEV_H
#define APEXOS_BLOCKDEV_H

#include <stdint.h>
#include <stddef.h>

#define SECTOR_SIZE 512u

/*
 * Минимальный интерфейс блочного устройства — ровно то, что нужно
 * FAT32-драйверу. read_sector/write_sector работают ровно с одним
 * сектором (SECTOR_SIZE байт) за вызов; более сложные операции
 * (множественное чтение, кэш) — на уровне вызывающего кода, не здесь.
 * Возвращают 0 при успехе, -1 при ошибке (в т.ч. lba вне диапазона).
 */
struct blockdev {
    uint64_t sector_count;
    int (*read_sector)(struct blockdev *dev, uint64_t lba, void *buf);
    int (*write_sector)(struct blockdev *dev, uint64_t lba, const void *buf);
    void *driver_data;
};

#endif /* APEXOS_BLOCKDEV_H */
