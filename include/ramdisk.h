#ifndef APEXOS_RAMDISK_H
#define APEXOS_RAMDISK_H

#include "blockdev.h"

/* ramdisk_init: выделяет и мапит через VMM+PMM фиксированный объём
   памяти под блочное устройство в RAM (см. RAMDISK_SIZE_BYTES в
   ramdisk.c), обнуляет его, возвращает готовый struct blockdev*.
   panic() при нехватке физической памяти — на этом этапе загрузки
   это нехватка настолько базового ресурса, что продолжать бессмысленно. */
struct blockdev *ramdisk_init(void);

#endif /* APEXOS_RAMDISK_H */
