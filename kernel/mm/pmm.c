#include "pmm.h"
#include "serial.h"
#include "panic.h"
#include <stddef.h>

/* Границы физического образа ядра — определены в linker.ld. Это
   символы, а не переменные: их АДРЕС и есть значение, поэтому
   объявляем как массивы и берём (uintptr_t)symbol. */
extern uint8_t _kernel_phys_start[];
extern uint8_t _kernel_phys_end[];

/*
 * Поддерживаем битовую карту на фиксированные 4 GiB физического
 * адресного пространства — этого с большим запасом хватает для hobby
 * OS на этом этапе (QEMU-тесты используют существенно меньше). Если
 * реальной RAM меньше, используется только соответствующий префикс
 * битовой карты — остаток остаётся помеченным "занято" и просто не
 * используется, лишней памяти это не тратит (весь массив — 128 KiB
 * статики в .bss, не зависит от реального объёма RAM).
 */
#define PMM_MAX_SUPPORTED_BYTES (4ull * 1024 * 1024 * 1024)
#define PMM_BITMAP_BITS  (PMM_MAX_SUPPORTED_BYTES / PMM_FRAME_SIZE)
#define PMM_BITMAP_BYTES (PMM_BITMAP_BITS / 8)

static uint8_t g_bitmap[PMM_BITMAP_BYTES];
static uint64_t g_total_frames = 0;
static uint64_t g_free_frames = 0;

static inline uint64_t frame_of(uint64_t addr) {
    return addr / PMM_FRAME_SIZE;
}

static inline int bitmap_test(uint64_t frame) {
    return (g_bitmap[frame / 8] >> (frame % 8)) & 1;
}

static inline void bitmap_set_used(uint64_t frame) {
    g_bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void bitmap_set_free(uint64_t frame) {
    g_bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

/* Помечает диапазон [start_addr, end_addr) занятым, безопасно обрезая
   по границам управляемой битовой карты — вызывающий код (например,
   регион из mmap с адресом за пределами 4 GiB) не должен приводить к
   записи за пределы g_bitmap. */
static void mark_range_used(uint64_t start_addr, uint64_t end_addr) {
    if (end_addr <= start_addr) {
        return;
    }
    uint64_t start_frame = frame_of(start_addr);
    /* округление вверх для конца диапазона */
    uint64_t end_frame = (end_addr + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    if (start_frame >= PMM_BITMAP_BITS) {
        return; /* полностью вне управляемого диапазона */
    }
    if (end_frame > PMM_BITMAP_BITS) {
        end_frame = PMM_BITMAP_BITS;
    }

    for (uint64_t f = start_frame; f < end_frame; f++) {
        if (!bitmap_test(f)) {
            g_free_frames--; /* был free, стал used */
        }
        bitmap_set_used(f);
    }
}

static void mark_range_free(uint64_t start_addr, uint64_t end_addr) {
    if (end_addr <= start_addr) {
        return;
    }
    /* Для "освобождения" (изначальной разметки available-региона)
       округляем ВНУТРЬ: частично занятый крайний фрейм не считаем
       свободным — иначе можно было бы отдать в аренду память, часть
       которой на самом деле принадлежит соседнему занятому региону. */
    uint64_t start_frame = (start_addr + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    uint64_t end_frame = end_addr / PMM_FRAME_SIZE;

    if (start_frame >= PMM_BITMAP_BITS) {
        return;
    }
    if (end_frame > PMM_BITMAP_BITS) {
        end_frame = PMM_BITMAP_BITS;
    }

    for (uint64_t f = start_frame; f < end_frame; f++) {
        if (bitmap_test(f)) {
            g_free_frames++;
        }
        bitmap_set_free(f);
    }
}

void pmm_init(const struct multiboot_tag_mmap *mmap_tag,
              uint32_t mb_info_phys, uint32_t mb_info_total_size,
              uint32_t module_start, uint32_t module_end) {
    if (mmap_tag == NULL) {
        panic("pmm_init: no memory map tag provided by bootloader");
    }
    if (mmap_tag->entry_size < sizeof(struct multiboot_mmap_entry)) {
        panic("pmm_init: mmap entry_size %u smaller than expected %u",
              mmap_tag->entry_size, (unsigned)sizeof(struct multiboot_mmap_entry));
    }

    /* Изначально ВСЁ занято — освобождаем только то, что явно
       перечислено как MULTIBOOT_MEMORY_AVAILABLE. Так безопаснее:
       неизвестный/непонятый регион остаётся недоступным для аллокатора,
       а не случайно выдаётся кому-то поверх, например, MMIO. */
    for (uint64_t i = 0; i < PMM_BITMAP_BYTES; i++) {
        g_bitmap[i] = 0xFF;
    }
    g_free_frames = 0;
    g_total_frames = 0;

    uint32_t entries_bytes = mmap_tag->size - (uint32_t)offsetof(struct multiboot_tag_mmap, entries);
    uint32_t entry_count = entries_bytes / mmap_tag->entry_size;
    uint64_t highest_available_end = 0;

    const uint8_t *entry_ptr = (const uint8_t *)mmap_tag->entries;
    for (uint32_t i = 0; i < entry_count; i++) {
        const struct multiboot_mmap_entry *entry =
            (const struct multiboot_mmap_entry *)(entry_ptr + (uint64_t)i * mmap_tag->entry_size);

        serial_printf("[pmm] mmap[%u]: addr=0x%lx len=0x%lx type=%u\n",
                      i, (unsigned long)entry->addr, (unsigned long)entry->len,
                      (unsigned)entry->type);

        if (entry->type != MULTIBOOT_MEMORY_AVAILABLE) {
            continue;
        }
        if (entry->len == 0) {
            continue;
        }

        uint64_t region_end = entry->addr + entry->len; /* len>0 и адреса реальны, переполнение здесь
                                                              означало бы region выходит за адресуемое
                                                              64-битное пространство — недостижимо для
                                                              реальной RAM, но проверим на всякий случай */
        if (region_end < entry->addr) {
            panic("pmm_init: mmap entry %u overflows 64-bit address space", i);
        }

        mark_range_free(entry->addr, region_end);
        if (region_end > highest_available_end) {
            highest_available_end = region_end;
        }
    }

    if (highest_available_end == 0) {
        panic("pmm_init: no available memory regions found in mmap");
    }

    g_total_frames = frame_of(highest_available_end);
    if (g_total_frames > PMM_BITMAP_BITS) {
        g_total_frames = PMM_BITMAP_BITS;
    }

    /* Явные резервы — эти диапазоны нельзя раздавать, даже если mmap
       формально пометил их как available (реально так и бывает: BIOS/
       GRUB не знают, где именно в "available" памяти лежит наше ядро). */
    mark_range_used(0, PMM_FRAME_SIZE); /* нулевая страница: IVT/BDA в реальном режиме */
    mark_range_used((uint64_t)(uintptr_t)_kernel_phys_start,
                     (uint64_t)(uintptr_t)_kernel_phys_end);
    mark_range_used(mb_info_phys, (uint64_t)mb_info_phys + mb_info_total_size);

    if (module_end > module_start) { /* 0,0 или битый диапазон — просто нечего резервировать */
        mark_range_used(module_start, module_end);
        serial_printf("[pmm] reserved multiboot2 module range [0x%x, 0x%x)\n",
                      module_start, module_end);
    }

    serial_printf("[pmm] total=%lu frames (~%lu MiB), free=%lu frames (~%lu MiB)\n",
                  (unsigned long)g_total_frames,
                  (unsigned long)(g_total_frames * PMM_FRAME_SIZE / (1024 * 1024)),
                  (unsigned long)g_free_frames,
                  (unsigned long)(g_free_frames * PMM_FRAME_SIZE / (1024 * 1024)));
}

uint64_t pmm_alloc_frame(void) {
    /* Линейный скан битовой карты — O(n) по количеству фреймов.
       Осознанно просто для этого этапа: свободный список фреймов
       (O(1) alloc/free) — оптимизация, которую стоит делать вместе с
       кучей ядра, а не раньше. Для нескольких сотен MiB тестовой RAM
       это не создаёт заметной задержки. */
    for (uint64_t frame = 0; frame < g_total_frames; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set_used(frame);
            g_free_frames--;
            return frame * PMM_FRAME_SIZE;
        }
    }
    return 0; /* нет свободных фреймов */
}

void pmm_free_frame(uint64_t phys_addr) {
    if (phys_addr % PMM_FRAME_SIZE != 0) {
        panic("pmm_free_frame: address 0x%lx is not frame-aligned", (unsigned long)phys_addr);
    }
    uint64_t frame = frame_of(phys_addr);
    if (frame >= g_total_frames) {
        panic("pmm_free_frame: frame %lu out of managed range (%lu total)",
              (unsigned long)frame, (unsigned long)g_total_frames);
    }
    if (!bitmap_test(frame)) {
        panic("pmm_free_frame: double free of frame %lu (0x%lx)",
              (unsigned long)frame, (unsigned long)phys_addr);
    }
    bitmap_set_free(frame);
    g_free_frames++;
}

uint64_t pmm_total_frames(void) { return g_total_frames; }
uint64_t pmm_free_frames(void) { return g_free_frames; }
