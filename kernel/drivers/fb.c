#include "fb.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include <stddef.h>

/* Отдельные virtual-диапазоны для реального framebuffer'а и backbuffer'а —
   не пересекаются ни с kheap (0xFFFFFFFFA0...), ни с чем-либо ещё. */
#define FB_VIRT_BASE       0xFFFFFFFFB0000000ull
#define BACKBUF_VIRT_BASE  0xFFFFFFFFC0000000ull

/* Разумный верхний предел размера кадра — отсекает откровенно битые
   значения pitch/height от бракованного или чужого тега, а не падаем
   на попытке замапить терабайты. 256 MiB с большим запасом покрывает
   любое реальное разрешение при 32bpp (например, 4K = ~32 MiB). */
#define FB_MAX_BYTES (256ull * 1024 * 1024)

static int g_available = 0;
static uint8_t *g_fb_ptr = NULL;
static uint8_t *g_backbuf_ptr = NULL;
static uint32_t g_width = 0;
static uint32_t g_height = 0;
static uint32_t g_pitch = 0;
static uint64_t g_frame_bytes = 0;

static uint8_t g_red_pos = 0, g_green_pos = 0, g_blue_pos = 0;

int fb_available(void) { return g_available; }
uint32_t fb_width(void) { return g_width; }
uint32_t fb_height(void) { return g_height; }

uint32_t fb_make_color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << g_red_pos) | ((uint32_t)g << g_green_pos) | ((uint32_t)b << g_blue_pos);
}

int fb_init(const struct multiboot_tag_framebuffer *tag) {
    if (tag == NULL) {
        serial_write("[fb] no framebuffer tag — graphics unavailable, serial log only\n");
        return -1;
    }

    size_t common_min_size = offsetof(struct multiboot_tag_framebuffer, framebuffer_red_field_position);
    if (tag->size < common_min_size) {
        serial_write("[fb] framebuffer tag smaller than the common struct — ignoring\n");
        return -1;
    }
    if (tag->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
        serial_printf("[fb] framebuffer_type=%u is not RGB (0/2=indexed/EGA text) — unsupported, "
                      "graphics unavailable\n", (unsigned)tag->framebuffer_type);
        return -1;
    }
    if (tag->framebuffer_bpp != 32) {
        serial_printf("[fb] framebuffer_bpp=%u, only 32bpp is supported — graphics unavailable\n",
                      (unsigned)tag->framebuffer_bpp);
        return -1;
    }
    if (tag->size < sizeof(struct multiboot_tag_framebuffer)) {
        serial_write("[fb] RGB framebuffer tag is missing color field-position info — ignoring\n");
        return -1;
    }
    if (tag->framebuffer_width == 0 || tag->framebuffer_height == 0 || tag->framebuffer_pitch == 0) {
        serial_write("[fb] framebuffer tag has zero width/height/pitch — ignoring\n");
        return -1;
    }

    uint64_t frame_bytes = (uint64_t)tag->framebuffer_pitch * tag->framebuffer_height;
    if (frame_bytes == 0 || frame_bytes > FB_MAX_BYTES) {
        serial_printf("[fb] framebuffer size %lu bytes looks implausible — ignoring\n",
                      (unsigned long)frame_bytes);
        return -1;
    }

    /* --- Мапим РЕАЛЬНЫЙ framebuffer (физический адрес от GRUB) --- */
    uint64_t phys_addr = tag->framebuffer_addr;
    uint64_t phys_page_start = phys_addr & ~(uint64_t)(VMM_PAGE_SIZE - 1);
    uint64_t phys_offset = phys_addr - phys_page_start;
    uint64_t total_bytes = phys_offset + frame_bytes;
    uint64_t num_pages = (total_bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;

    for (uint64_t i = 0; i < num_pages; i++) {
        vmm_map(FB_VIRT_BASE + i * VMM_PAGE_SIZE, phys_page_start + i * VMM_PAGE_SIZE,
                VMM_PRESENT | VMM_WRITABLE);
    }
    g_fb_ptr = (uint8_t *)(uintptr_t)(FB_VIRT_BASE + phys_offset);

    /* --- Выделяем и мапим backbuffer того же размера --- */
    uint64_t backbuf_pages = (frame_bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    for (uint64_t i = 0; i < backbuf_pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            serial_write("[fb] out of physical memory allocating backbuffer — graphics unavailable\n");
            return -1;
        }
        vmm_map(BACKBUF_VIRT_BASE + i * VMM_PAGE_SIZE, frame, VMM_PRESENT | VMM_WRITABLE);
    }
    g_backbuf_ptr = (uint8_t *)(uintptr_t)BACKBUF_VIRT_BASE;

    g_width = tag->framebuffer_width;
    g_height = tag->framebuffer_height;
    g_pitch = tag->framebuffer_pitch;
    g_frame_bytes = frame_bytes;
    g_red_pos = tag->framebuffer_red_field_position;
    g_green_pos = tag->framebuffer_green_field_position;
    g_blue_pos = tag->framebuffer_blue_field_position;
    g_available = 1;

    serial_printf("[fb] initialized: %ux%u, pitch=%u, 32bpp, phys=0x%lx, "
                  "R@%u G@%u B@%u, backbuffer=%lu KiB\n",
                  g_width, g_height, g_pitch, (unsigned long)phys_addr,
                  (unsigned)g_red_pos, (unsigned)g_green_pos, (unsigned)g_blue_pos,
                  (unsigned long)(frame_bytes / 1024));
    return 0;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_available || x >= g_width || y >= g_height) {
        return; /* тихий no-op за границами — как и положено примитиву рисования */
    }
    uint32_t *row = (uint32_t *)(g_backbuf_ptr + (uint64_t)y * g_pitch);
    row[x] = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!g_available) {
        return;
    }
    /* Клэмпим прямоугольник к границам экрана — вызывающий код может
       передать w/h, выходящие за пределы, это не должно писать за
       backbuffer. */
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;
    if (x_end > g_width || x_end < x) x_end = g_width;   /* '< x' ловит переполнение x+w */
    if (y_end > g_height || y_end < y) y_end = g_height;

    for (uint32_t py = y; py < y_end; py++) {
        uint32_t *row = (uint32_t *)(g_backbuf_ptr + (uint64_t)py * g_pitch);
        for (uint32_t px = x; px < x_end; px++) {
            row[px] = color;
        }
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, g_width, g_height, color);
}

void fb_present(void) {
    if (!g_available) {
        return;
    }
    memcpy(g_fb_ptr, g_backbuf_ptr, g_frame_bytes);
}

void fb_scroll_up(uint32_t pixel_rows, uint32_t fill_color) {
    if (!g_available) {
        return;
    }
    if (pixel_rows >= g_height) {
        fb_clear(fill_color);
        return;
    }
    if (pixel_rows == 0) {
        return;
    }
    uint64_t move_bytes = (uint64_t)(g_height - pixel_rows) * g_pitch;
    memmove(g_backbuf_ptr, g_backbuf_ptr + (uint64_t)pixel_rows * g_pitch, move_bytes);
    fb_fill_rect(0, g_height - pixel_rows, g_width, pixel_rows, fill_color);
}
