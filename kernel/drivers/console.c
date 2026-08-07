#include "console.h"
#include "fb.h"
#include "font8x16.h"
#include "fmt.h"
#include <stddef.h>

static uint32_t g_cols = 0, g_rows = 0;
static uint32_t g_cursor_x = 0, g_cursor_y = 0;
static uint32_t g_fg = 0, g_bg = 0;
static int g_console_available = 0;

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    uint32_t px = col * FONT8X16_WIDTH;
    uint32_t py = row * FONT8X16_HEIGHT;

    const uint8_t *glyph;
    if (c >= FONT8X16_FIRST_CHAR && c <= FONT8X16_LAST_CHAR) {
        glyph = font8x16_data[(unsigned char)c - FONT8X16_FIRST_CHAR];
    } else {
        /* Символ вне покрытого диапазона (не 32..126) — печатаем '?',
           а не пропускаем и не рисуем случайный мусор из памяти. */
        glyph = font8x16_data['?' - FONT8X16_FIRST_CHAR];
    }

    for (uint32_t y = 0; y < FONT8X16_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT8X16_WIDTH; x++) {
            int on = (bits >> (7 - x)) & 1;
            fb_put_pixel(px + x, py + y, on ? g_fg : g_bg);
        }
    }
}

void console_init(void) {
    g_console_available = fb_available();
    if (!g_console_available) {
        return;
    }
    g_cols = fb_width() / FONT8X16_WIDTH;
    g_rows = fb_height() / FONT8X16_HEIGHT;
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_fg = fb_make_color(0xC0, 0xC0, 0xC0);
    g_bg = fb_make_color(0x00, 0x00, 0x00);
    console_clear();
}

int console_available(void) { return g_console_available; }

static void newline(void) {
    g_cursor_x = 0;
    g_cursor_y++;
    if (g_cursor_y >= g_rows) {
        fb_scroll_up(FONT8X16_HEIGHT, g_bg);
        g_cursor_y = g_rows - 1;
    }
}

/* putc_internal: рисует символ, но НЕ делает fb_present() — используется
   write/printf, чтобы не гонять memcpy всего кадра на каждый символ
   длинной строки. console_putc (публичная, для живого эха при вводе с
   клавиатуры) делает present сразу. */
static void putc_internal(char c) {
    if (!g_console_available) {
        return;
    }
    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        g_cursor_x = 0;
        return;
    }
    if (c == '\b') {
        if (g_cursor_x > 0) {
            g_cursor_x--;
            draw_glyph(g_cursor_x, g_cursor_y, ' ');
        }
        return;
    }
    if (c == '\t') {
        uint32_t next_stop = (g_cursor_x / 8 + 1) * 8;
        while (g_cursor_x < next_stop && g_cursor_x < g_cols) {
            draw_glyph(g_cursor_x, g_cursor_y, ' ');
            g_cursor_x++;
        }
        return;
    }

    draw_glyph(g_cursor_x, g_cursor_y, c);
    g_cursor_x++;
    if (g_cursor_x >= g_cols) {
        newline();
    }
}

void console_putc(char c) {
    putc_internal(c);
    fb_present();
}

void console_putc_np(char c) {
    putc_internal(c);
}

void console_backspace(void) {
    putc_internal('\b');
    fb_present();
}

void console_write(const char *s) {
    if (!g_console_available) {
        return;
    }
    while (*s) {
        putc_internal(*s++);
    }
    fb_present();
}

void console_write_len(const char *s, size_t len) {
    if (!g_console_available) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        putc_internal(s[i]);
    }
    fb_present();
}

static void console_putc_cb(void *ctx, char c) {
    (void)ctx;
    putc_internal(c);
}

void console_vprintf(const char *fmt, va_list args) {
    if (!g_console_available) {
        return;
    }
    vfmt(console_putc_cb, NULL, fmt, args);
    fb_present();
}

void console_printf(const char *fmt, ...) {
    if (!g_console_available) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfmt(console_putc_cb, NULL, fmt, args);
    va_end(args);
    fb_present();
}

void console_clear(void) {
    if (!g_console_available) {
        return;
    }
    fb_clear(g_bg);
    g_cursor_x = 0;
    g_cursor_y = 0;
    fb_present();
}

void console_write_len_color(const char *s, size_t len, uint32_t color) {
    if (!g_console_available) {
        return;
    }
    uint32_t saved_fg = g_fg;
    g_fg = color;
    for (size_t i = 0; i < len; i++) {
        putc_internal(s[i]);
    }
    g_fg = saved_fg;
    /* НЕ вызываем fb_present() здесь -- эта функция вызывается по разу
       НА КАЖДЫЙ ТОКЕН из highlight.c (а для пунктуации/пробелов — по
       разу на КАЖДЫЙ СИМВОЛ). Раньше здесь стоял fb_present(), и
       полноэкранная перерисовка (editor.c::render(), вызываемая на
       КАЖДОЕ нажатие клавиши) могла делать сотни полных memcpy кадра
       на одну перерисовку — отсюда были чудовищные задержки ввода в
       nano (буквы проявлялись через минуты). Теперь вызывающий код
       (render(), cmd_cat) сам делает ОДИН console_present() в конце
       всего пакета вывода. */
}

void console_present(void) {
    if (!g_console_available) {
        return;
    }
    fb_present();
}

void console_draw_cursor(uint32_t col, uint32_t row, uint32_t color) {
    if (!g_console_available || col >= g_cols || row >= g_rows) {
        return;
    }
    uint32_t px = col * FONT8X16_WIDTH;
    uint32_t py = row * FONT8X16_HEIGHT;
    /* Полоска в 2 пикселя снизу ячейки — классический "underline"
       текстовый курсор, не перекрывает сам символ под ним.
       Без fb_present() здесь -- вызывается один раз в конце render(),
       которая сама делает единственный console_present() в самом конце. */
    fb_fill_rect(px, py + FONT8X16_HEIGHT - 2, FONT8X16_WIDTH, 2, color);
}

uint32_t console_cols(void) { return g_console_available ? g_cols : 0; }
uint32_t console_rows(void) { return g_console_available ? g_rows : 0; }
