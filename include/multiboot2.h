#ifndef APEXOS_MULTIBOOT2_H
#define APEXOS_MULTIBOOT2_H

#include <stdint.h>

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

#define MULTIBOOT_TAG_TYPE_END         0
#define MULTIBOOT_TAG_TYPE_CMDLINE     1
#define MULTIBOOT_TAG_TYPE_MODULE      3
#define MULTIBOOT_TAG_TYPE_MMAP        6
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER 8

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct multiboot_info_header {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed));

struct multiboot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry entries[];
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

/* Модуль, загруженный GRUB'ом вместе с ядром (директива module2 в
   grub.cfg) — physical [mod_start, mod_end). Используется для теста
   ELF64-загрузчика/userspace ДО того, как у нас появится настоящая
   файловая система: это тот же механизм, которым реальные hobby OS
   передают initrd, честно, а не костыль в обход спецификации. */
struct multiboot_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];
} __attribute__((packed));

#define MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED  0
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB      1
#define MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT 2

/* framebuffer tag (type=8). Поля цвета (red/green/blue position+mask)
   валидны только когда framebuffer_type == RGB — для остальных типов
   мы всё равно отказываемся работать (см. fb_init), но структуру
   держим полной, а не только "common"-частью, чтобы не подсматривать
   за границы через смещения руками. */
struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
    uint8_t  framebuffer_red_field_position;
    uint8_t  framebuffer_red_mask_size;
    uint8_t  framebuffer_green_field_position;
    uint8_t  framebuffer_green_mask_size;
    uint8_t  framebuffer_blue_field_position;
    uint8_t  framebuffer_blue_mask_size;
} __attribute__((packed));

#endif /* APEXOS_MULTIBOOT2_H */
