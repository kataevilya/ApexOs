#include "highlight.h"
#include "console.h"
#include "serial.h"
#include "fb.h"
#include "string.h"

static uint32_t g_color_default;
static uint32_t g_color_keyword;
static uint32_t g_color_string;
static uint32_t g_color_comment;
static uint32_t g_color_number;
static uint32_t g_color_preproc;
static int g_colors_ready = 0;

/* highlight_init: вычисляет палитру через fb_make_color() (учитывает
   реальный порядок R/G/B каналов железа) — вызывается один раз из
   main.c после fb_init()/console_init(). Безопасно вызывать и без
   доступного framebuffer: console_write_len_color() тогда просто
   no-op, портить нечего. */
void highlight_init(void) {
    g_color_default = fb_make_color(0xC0, 0xC0, 0xC0);
    g_color_keyword  = fb_make_color(0x50, 0x90, 0xFF);
    g_color_string   = fb_make_color(0x40, 0xC0, 0x40);
    g_color_comment  = fb_make_color(0x80, 0x80, 0x80);
    g_color_number   = fb_make_color(0xE0, 0xA0, 0x40);
    g_color_preproc  = fb_make_color(0xE0, 0x60, 0x20);
    g_colors_ready = 1;
}

static const char *const KEYWORDS[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "int", "long", "register",
    "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
    "union", "unsigned", "void", "volatile", "while", "inline", "restrict", "_Bool",
    NULL
};

static int is_ident_start(char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_keyword(const char *s, size_t len) {
    for (int i = 0; KEYWORDS[i] != NULL; i++) {
        size_t klen = strlen(KEYWORDS[i]);
        if (klen == len && strncmp(s, KEYWORDS[i], len) == 0) {
            return 1;
        }
    }
    return 0;
}

static void emit(const char *s, size_t len, uint32_t color) {
    console_write_len_color(s, len, color);
    serial_write_len(s, len); /* serial — без цвета, обычный текст */
}

void highlight_state_reset(struct highlight_state *st) {
    st->in_block_comment = 0;
}

void highlight_print_c_line(struct highlight_state *st, const char *line) {
    if (!g_colors_ready) {
        /* highlight_init() не вызывался — печатаем как обычный текст,
           а не молча используем нулевые цвета (которые могли бы
           случайно совпасть с фоном и сделать текст невидимым). */
        console_write(line);
        serial_write(line);
        return;
    }

    size_t len = strlen(line);
    size_t i = 0;

    if (st->in_block_comment) {
        size_t start = 0;
        int closed = 0;
        while (i < len) {
            if (line[i] == '*' && i + 1 < len && line[i + 1] == '/') {
                i += 2;
                closed = 1;
                break;
            }
            i++;
        }
        emit(line + start, i - start, g_color_comment);
        if (!closed) {
            return; /* вся строка осталась внутри блочного комментария */
        }
        st->in_block_comment = 0;
    }

    /* Препроцессорная директива — вся строка одним цветом (только если
       мы не только что вышли из блочного комментария с предыдущей строки). */
    {
        size_t j = i;
        while (j < len && (line[j] == ' ' || line[j] == '\t')) {
            j++;
        }
        if (j < len && line[j] == '#') {
            emit(line + i, len - i, g_color_preproc);
            return;
        }
    }

    while (i < len) {
        char c = line[i];

        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            emit(line + i, len - i, g_color_comment);
            return;
        }
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            size_t start = i;
            i += 2;
            while (i < len && !(line[i] == '*' && i + 1 < len && line[i + 1] == '/')) {
                i++;
            }
            if (i < len) {
                i += 2;
            } else {
                st->in_block_comment = 1;
                i = len;
            }
            emit(line + start, i - start, g_color_comment);
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            size_t start = i;
            i++;
            while (i < len && line[i] != quote) {
                if (line[i] == '\\' && i + 1 < len) {
                    i++;
                }
                i++;
            }
            if (i < len) {
                i++;
            }
            emit(line + start, i - start, g_color_string);
            continue;
        }
        if (is_digit(c)) {
            size_t start = i;
            while (i < len && (is_digit(line[i]) || line[i] == '.' ||
                                (line[i] >= 'a' && line[i] <= 'f') ||
                                (line[i] >= 'A' && line[i] <= 'F') ||
                                line[i] == 'x' || line[i] == 'X' ||
                                line[i] == 'u' || line[i] == 'U' ||
                                line[i] == 'l' || line[i] == 'L')) {
                i++;
            }
            emit(line + start, i - start, g_color_number);
            continue;
        }
        if (is_ident_start(c)) {
            size_t start = i;
            while (i < len && is_ident_char(line[i])) {
                i++;
            }
            uint32_t color = is_keyword(line + start, i - start) ? g_color_keyword : g_color_default;
            emit(line + start, i - start, color);
            continue;
        }

        emit(line + i, 1, g_color_default);
        i++;
    }
}
