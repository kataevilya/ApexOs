#include "ramdisk.h"
#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "serial.h"
#include "string.h"

/* Отдельный virtual-диапазон, не пересекается с kheap/framebuffer/backbuffer. */
#define RAMDISK_VIRT_BASE   0xFFFFFFFFD0000000ull
#define RAMDISK_SIZE_BYTES  (8ull * 1024 * 1024) /* 8 MiB — см. честную оговорку в fat32.c
                                                      про порог в 65525 кластеров */
#define RAMDISK_SECTORS     (RAMDISK_SIZE_BYTES / SECTOR_SIZE)

static uint8_t *g_ramdisk_ptr = NULL;
static struct blockdev g_dev;

static int ramdisk_read_sector(struct blockdev *dev, uint64_t lba, void *buf) {
    (void)dev;
    if (lba >= RAMDISK_SECTORS) {
        return -1;
    }
    memcpy(buf, g_ramdisk_ptr + lba * SECTOR_SIZE, SECTOR_SIZE);
    return 0;
}

static int ramdisk_write_sector(struct blockdev *dev, uint64_t lba, const void *buf) {
    (void)dev;
    if (lba >= RAMDISK_SECTORS) {
        return -1;
    }
    memcpy(g_ramdisk_ptr + lba * SECTOR_SIZE, buf, SECTOR_SIZE);
    return 0;
}

struct blockdev *ramdisk_init(void) {
    uint64_t pages = RAMDISK_SIZE_BYTES / VMM_PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            panic("ramdisk_init: out of physical memory (%lu MiB requested)",
                  (unsigned long)(RAMDISK_SIZE_BYTES / 1024 / 1024));
        }
        vmm_map(RAMDISK_VIRT_BASE + i * VMM_PAGE_SIZE, frame, VMM_PRESENT | VMM_WRITABLE);
    }
    g_ramdisk_ptr = (uint8_t *)(uintptr_t)RAMDISK_VIRT_BASE;
    memset(g_ramdisk_ptr, 0, RAMDISK_SIZE_BYTES);

    g_dev.sector_count = RAMDISK_SECTORS;
    g_dev.read_sector = ramdisk_read_sector;
    g_dev.write_sector = ramdisk_write_sector;
    g_dev.driver_data = NULL;

    serial_printf("[ramdisk] initialized: %lu MiB (%lu sectors)\n",
                  (unsigned long)(RAMDISK_SIZE_BYTES / 1024 / 1024),
                  (unsigned long)RAMDISK_SECTORS);
    return &g_dev;
}
