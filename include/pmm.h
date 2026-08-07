#ifndef APEXOS_PMM_H
#define APEXOS_PMM_H

#include <stdint.h>
#include <stddef.h>
#include "multiboot2.h"

#define PMM_FRAME_SIZE 4096u

/*
 * pmm_init: строит битовую карту физических фреймов на основе Multiboot2
 * memory map. Помечает занятыми:
 *   - фреймы, не входящие ни в один MULTIBOOT_MEMORY_AVAILABLE регион;
 *   - физический образ самого ядра (_kernel_phys_start.._kernel_phys_end,
 *     см. linker.ld);
 *   - страницу(и), где реально лежит сама Multiboot2 info-структура;
 *   - страницу(и) Multiboot2-модуля (module_start..module_end), если он
 *     есть (module_start==module_end==0, если модуля нет) — БЕЗ этого
 *     резерва любая последующая аллокация (heap, framebuffer backbuffer,
 *     сам ELF-загрузчик) могла бы получить те же физические страницы и
 *     молча испортить модуль до того, как elf64_load() его прочитает;
 *   - саму битовую карту (она выделяется статически в BSS, но всё
 *     равно резервируется явно, а не "потому что и так не тронут").
 *
 * mmap_tag должен быть валидным tag'ом из multiboot2 info (см. main.c) —
 * pmm_init НЕ парсит info-структуру заново, чтобы не дублировать логику
 * валидации границ, которая уже есть в parse_multiboot_tags().
 */
void pmm_init(const struct multiboot_tag_mmap *mmap_tag,
              uint32_t mb_info_phys, uint32_t mb_info_total_size,
              uint32_t module_start, uint32_t module_end);

/* Выделяет один физический фрейм (4 KiB), возвращает его физический
   адрес. Возвращает 0 при нехватке памяти — 0 никогда не является
   валидным выделяемым фреймом (страница 0 всегда зарезервирована),
   поэтому 0 однозначно читается как "ошибка", без отдельного out-параметра. */
uint64_t pmm_alloc_frame(void);

/* Освобождает ранее выделенный фрейм. panic() при попытке освободить
   фрейм вне управляемого диапазона или невыровненный адрес — это
   всегда программная ошибка вызывающего кода, а не штатная ситуация. */
void pmm_free_frame(uint64_t phys_addr);

uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);

#endif /* APEXOS_PMM_H */
