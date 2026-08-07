#ifndef APEXOS_FB_H
#define APEXOS_FB_H

#include <stdint.h>
#include "multiboot2.h"

/*
 * fb_init: валидирует framebuffer-тег от GRUB, мапит реальный
 * (физический) framebuffer через vmm_map() в собственный virtual
 * диапазон, выделяет backbuffer того же размера (тоже через vmm_map,
 * не через kmalloc — несколько MiB, незачем занимать limited kheap).
 *
 * Возвращает 0 при успехе. Возвращает -1 (не panic!) если:
 *   - тега нет вообще;
 *   - framebuffer_type != RGB (например, EGA text или indexed — не
 *     поддерживаем, честно отказываемся, а не рисуем мусор);
 *   - framebuffer_bpp != 32 (мы запрашивали 32 в Multiboot2-заголовке,
 *     но GRUB не обязан это дать, если железо/VBE не поддерживает).
 * Отсутствие графики — не повод останавливать всё ядро (serial-лог
 * по-прежнему работает), поэтому это не panic().
 */
int fb_init(const struct multiboot_tag_framebuffer *tag);

/* 1, если fb_init() отработал успешно и рисование доступно. */
int fb_available(void);

uint32_t fb_width(void);
uint32_t fb_height(void);

/* Собирает 32-битное значение пикселя с учётом реального порядка
   R/G/B, который сообщило железо (red/green/blue field position) —
   НЕ предполагаем фиксированный 0x00RRGGBB. */
uint32_t fb_make_color(uint8_t r, uint8_t g, uint8_t b);

/* Все функции ниже пишут в BACKBUFFER, не в реальный framebuffer —
   на экране ничего не изменится, пока не будет вызван fb_present(). */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);

/* fb_present: копирует весь backbuffer в реальный framebuffer одним
   проходом — единственное место, где что-либо реально попадает на
   экран. Без этого вызова fb_clear/fb_put_pixel/fb_fill_rect не имеют
   видимого эффекта (что и есть смысл двойной буферизации — никакого
   мерцания промежуточных состояний кадра). */
void fb_present(void);

/* fb_scroll_up: сдвигает содержимое backbuffer вверх на pixel_rows
   строк пикселей (memmove), новую полосу внизу заполняет fill_color.
   Используется console.c для прокрутки текста. Если pixel_rows >=
   высоты экрана — эквивалентно fb_clear(fill_color). */
void fb_scroll_up(uint32_t pixel_rows, uint32_t fill_color);

#endif /* APEXOS_FB_H */
