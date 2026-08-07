#include "editor.h"
#include "console.h"
#include "serial.h"
#include "keyboard.h"
#include "string.h"
#include "kheap.h"
#include "highlight.h"
#include "fb.h"
#include "elf64.h"
#include "process.h"
#include <stddef.h>

#define EDITOR_MAX_LINES    512
#define EDITOR_MAX_LINE_LEN 200
#define EDITOR_IO_BUF_SIZE  (64u * 1024)

struct editor_state {
    char lines[EDITOR_MAX_LINES][EDITOR_MAX_LINE_LEN];
    int num_lines;
    int cursor_row, cursor_col;
    int scroll_row;
    int modified;
};

static void ed_status_write(const char *s) {
    console_write(s);
    serial_write(s);
}

static void editor_load(struct editor_state *ed, uint32_t dir_cluster,
                         const char name83[FAT32_NAME_LEN]) {
    ed->num_lines = 1;
    ed->lines[0][0] = '\0';
    ed->cursor_row = 0;
    ed->cursor_col = 0;
    ed->scroll_row = 0;
    ed->modified = 0;

    char *buf = (char *)kmalloc(EDITOR_IO_BUF_SIZE);
    if (buf == NULL) {
        return; /* нет памяти — остаёмся с пустым буфером, не падаем */
    }
    uint32_t real_size = 0;
    if (fat32_read_file(dir_cluster, name83, buf, EDITOR_IO_BUF_SIZE - 1, &real_size) != 0) {
        kfree(buf);
        return; /* файла нет — это новый файл, пустой буфер ожидаем */
    }
    uint32_t shown = (real_size < EDITOR_IO_BUF_SIZE - 1) ? real_size : EDITOR_IO_BUF_SIZE - 1;

    int line_idx = 0;
    size_t col = 0;
    for (uint32_t i = 0; i < shown; i++) {
        if (line_idx >= EDITOR_MAX_LINES - 1 && col >= EDITOR_MAX_LINE_LEN - 1) {
            break; /* файл больше, чем редактор поддерживает — честно обрезаем */
        }
        char c = buf[i];
        if (c == '\n') {
            ed->lines[line_idx][col] = '\0';
            if (line_idx < EDITOR_MAX_LINES - 1) {
                line_idx++;
            }
            col = 0;
            continue;
        }
        if (col < EDITOR_MAX_LINE_LEN - 1) {
            ed->lines[line_idx][col++] = c;
        }
    }
    ed->lines[line_idx][col] = '\0';
    ed->num_lines = line_idx + 1;
    kfree(buf);
}

static int editor_save(struct editor_state *ed, uint32_t dir_cluster,
                        const char name83[FAT32_NAME_LEN]) {
    char *buf = (char *)kmalloc(EDITOR_IO_BUF_SIZE);
    if (buf == NULL) {
        return -1;
    }
    size_t total = 0;
    for (int i = 0; i < ed->num_lines; i++) {
        size_t len = strlen(ed->lines[i]);
        if (total + len + 1 >= EDITOR_IO_BUF_SIZE) {
            break; /* лимит буфера сохранения — честно обрезаем остаток */
        }
        memcpy(buf + total, ed->lines[i], len);
        total += len;
        buf[total++] = '\n';
    }
    int rc = fat32_write_file(dir_cluster, name83, buf, (uint32_t)total);
    kfree(buf);
    return rc;
}

static char g_run_status_buf[64];

static const char *editor_try_run(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN]) {
    char *buf = (char *)kmalloc(EDITOR_IO_BUF_SIZE);
    if (buf == NULL) {
        return "run: out of memory";
    }
    uint32_t real_size = 0;
    if (fat32_read_file(dir_cluster, name83, buf, EDITOR_IO_BUF_SIZE, &real_size) != 0) {
        kfree(buf);
        return "run: could not read file";
    }
    if (real_size < 4 || (unsigned char)buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        kfree(buf);
        return "run: not an ELF64 binary -- ApexOS has no C compiler; "
               "build it on your host machine and copy the .elf here";
    }

    uint64_t entry = 0, user_stack = 0;
    if (elf64_load(buf, real_size, &entry, &user_stack) != 0) {
        kfree(buf);
        return "run: ELF64 load failed (see serial log)";
    }
    kfree(buf);

    int exit_code = process_run(entry, user_stack);
    /* Статический буфер: используется ровно один кадр рендера сразу
       после вызова, до следующего editor_try_run — переписывать его
       заранее некому. */
    g_run_status_buf[0] = '\0';
    const char *prefix = "exited with code ";
    size_t i = 0;
    while (prefix[i] && i < sizeof(g_run_status_buf) - 1) { g_run_status_buf[i] = prefix[i]; i++; }
    /* маленький встроенный itoa — не тянуть весь fmt.c ради одного числа в статусбаре */
    char digits[12];
    int dpos = 0;
    int v = exit_code;
    int neg = v < 0;
    unsigned int uv = neg ? (unsigned int)(-v) : (unsigned int)v;
    if (uv == 0) { digits[dpos++] = '0'; }
    while (uv > 0 && dpos < (int)sizeof(digits)) { digits[dpos++] = (char)('0' + uv % 10); uv /= 10; }
    if (neg && i < sizeof(g_run_status_buf) - 1) { g_run_status_buf[i++] = '-'; }
    while (dpos > 0 && i < sizeof(g_run_status_buf) - 1) { g_run_status_buf[i++] = digits[--dpos]; }
    g_run_status_buf[i] = '\0';
    return g_run_status_buf;
}

static void render(struct editor_state *ed, const char *filename, const char *status) {
    uint32_t rows = console_rows();
    uint32_t cols = console_cols();
    if (rows == 0) {
        return; /* нет экрана — редактору рисовать некуда, но не падаем */
    }
    uint32_t content_rows = rows - 1; /* последняя строка экрана — статусбар */

    if ((uint32_t)ed->cursor_row < (uint32_t)ed->scroll_row) {
        ed->scroll_row = ed->cursor_row;
    }
    if (ed->cursor_row >= ed->scroll_row + (int)content_rows) {
        ed->scroll_row = ed->cursor_row - (int)content_rows + 1;
    }

    console_clear();

    struct highlight_state hl;
    highlight_state_reset(&hl);
    for (uint32_t r = 0; r < content_rows; r++) {
        int line_idx = ed->scroll_row + (int)r;
        if (line_idx < ed->num_lines) {
            char clipped[EDITOR_MAX_LINE_LEN];
            size_t len = strlen(ed->lines[line_idx]);
            size_t max_show = (cols > 0) ? (size_t)(cols - 1) : 0; /* -1: не даём авто-переносу консоли добавить лишнюю строку */
            size_t show_len = (len < max_show) ? len : max_show;
            memcpy(clipped, ed->lines[line_idx], show_len);
            clipped[show_len] = '\0';
            highlight_print_c_line(&hl, clipped);
        }
        console_putc_np('\n');
    }

    console_printf("-- %s%s -- F2 Save  Esc Exit  Ctrl+R Save+Run -- %s",
                   filename, ed->modified ? " [modified]" : "", status ? status : "");

    uint32_t cur_row = (uint32_t)(ed->cursor_row - ed->scroll_row);
    uint32_t cur_col = (uint32_t)ed->cursor_col;
    if (cur_col < cols && cur_row < content_rows) {
        console_draw_cursor(cur_col, cur_row, fb_make_color(0xFF, 0xFF, 0x00));
    }
    console_present(); /* ОДИН раз на весь кадр, а не по разу на токен/строку -- см. console_write_len_color */
}

static void clamp_cursor_col(struct editor_state *ed) {
    int len = (int)strlen(ed->lines[ed->cursor_row]);
    if (ed->cursor_col > len) {
        ed->cursor_col = len;
    }
}

static void editor_insert_char(struct editor_state *ed, char c) {
    char *line = ed->lines[ed->cursor_row];
    int len = (int)strlen(line);
    if (len + 1 >= EDITOR_MAX_LINE_LEN) {
        return; /* строка на пределе длины — честно игнорируем ввод */
    }
    memmove(line + ed->cursor_col + 1, line + ed->cursor_col, (size_t)(len - ed->cursor_col + 1));
    line[ed->cursor_col] = c;
    ed->cursor_col++;
    ed->modified = 1;
}

static void editor_backspace(struct editor_state *ed) {
    if (ed->cursor_col > 0) {
        char *line = ed->lines[ed->cursor_row];
        int len = (int)strlen(line);
        memmove(line + ed->cursor_col - 1, line + ed->cursor_col, (size_t)(len - ed->cursor_col + 1));
        ed->cursor_col--;
        ed->modified = 1;
        return;
    }
    if (ed->cursor_row > 0) {
        char *prev = ed->lines[ed->cursor_row - 1];
        char *cur = ed->lines[ed->cursor_row];
        int prev_len = (int)strlen(prev);
        int cur_len = (int)strlen(cur);
        if (prev_len + cur_len >= EDITOR_MAX_LINE_LEN) {
            return; /* не помещается при слиянии — честно ничего не делаем */
        }
        memcpy(prev + prev_len, cur, (size_t)(cur_len + 1));
        for (int i = ed->cursor_row; i < ed->num_lines - 1; i++) {
            memcpy(ed->lines[i], ed->lines[i + 1], EDITOR_MAX_LINE_LEN);
        }
        ed->num_lines--;
        ed->cursor_row--;
        ed->cursor_col = prev_len;
        ed->modified = 1;
    }
}

static void editor_delete_forward(struct editor_state *ed) {
    char *line = ed->lines[ed->cursor_row];
    int len = (int)strlen(line);
    if (ed->cursor_col < len) {
        memmove(line + ed->cursor_col, line + ed->cursor_col + 1, (size_t)(len - ed->cursor_col));
        ed->modified = 1;
        return;
    }
    if (ed->cursor_row < ed->num_lines - 1) {
        char *next = ed->lines[ed->cursor_row + 1];
        int next_len = (int)strlen(next);
        if (len + next_len >= EDITOR_MAX_LINE_LEN) {
            return;
        }
        memcpy(line + len, next, (size_t)(next_len + 1));
        for (int i = ed->cursor_row + 1; i < ed->num_lines - 1; i++) {
            memcpy(ed->lines[i], ed->lines[i + 1], EDITOR_MAX_LINE_LEN);
        }
        ed->num_lines--;
        ed->modified = 1;
    }
}

static void editor_insert_newline(struct editor_state *ed) {
    if (ed->num_lines >= EDITOR_MAX_LINES) {
        return; /* лимит строк — честно игнорируем Enter */
    }
    char *line = ed->lines[ed->cursor_row];
    for (int i = ed->num_lines; i > ed->cursor_row + 1; i--) {
        memcpy(ed->lines[i], ed->lines[i - 1], EDITOR_MAX_LINE_LEN);
    }
    ed->num_lines++;
    /* strcpy_safe тут не нужен — источник заведомо короче лимита (это
       хвост существующей валидной строки), но проверяем длину явно. */
    size_t tail_len = strlen(line + ed->cursor_col);
    if (tail_len >= EDITOR_MAX_LINE_LEN) {
        tail_len = EDITOR_MAX_LINE_LEN - 1;
    }
    memcpy(ed->lines[ed->cursor_row + 1], line + ed->cursor_col, tail_len);
    ed->lines[ed->cursor_row + 1][tail_len] = '\0';
    line[ed->cursor_col] = '\0';
    ed->cursor_row++;
    ed->cursor_col = 0;
    ed->modified = 1;
}

void editor_run(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN], const char *display_name) {
    struct editor_state *ed = (struct editor_state *)kmalloc(sizeof(struct editor_state));
    if (ed == NULL) {
        ed_status_write("editor: out of memory\n");
        return;
    }
    editor_load(ed, dir_cluster, name83);

    const char *status = "";
    while (1) {
        render(ed, display_name, status);
        status = "";

        int ev;
        do {
            ev = keyboard_read_key();
            if (ev == KEY_NONE) {
                __asm__ volatile ("hlt");
            }
        } while (ev == KEY_NONE);

        if (ev == 27) { /* Esc */
            kfree(ed);
            return;
        }

        switch (ev) {
            case KEY_UP:
                if (ed->cursor_row > 0) { ed->cursor_row--; clamp_cursor_col(ed); }
                break;
            case KEY_DOWN:
                if (ed->cursor_row < ed->num_lines - 1) { ed->cursor_row++; clamp_cursor_col(ed); }
                break;
            case KEY_LEFT:
                if (ed->cursor_col > 0) {
                    ed->cursor_col--;
                } else if (ed->cursor_row > 0) {
                    ed->cursor_row--;
                    ed->cursor_col = (int)strlen(ed->lines[ed->cursor_row]);
                }
                break;
            case KEY_RIGHT: {
                int len = (int)strlen(ed->lines[ed->cursor_row]);
                if (ed->cursor_col < len) {
                    ed->cursor_col++;
                } else if (ed->cursor_row < ed->num_lines - 1) {
                    ed->cursor_row++;
                    ed->cursor_col = 0;
                }
                break;
            }
            case KEY_HOME:
                ed->cursor_col = 0;
                break;
            case KEY_END:
                ed->cursor_col = (int)strlen(ed->lines[ed->cursor_row]);
                break;
            case KEY_DELETE:
                editor_delete_forward(ed);
                break;
            case KEY_F2:
                status = (editor_save(ed, dir_cluster, name83) == 0) ? "saved" : "SAVE FAILED";
                ed->modified = (status[0] == 's') ? 0 : ed->modified;
                break;
            case KEY_CTRL_R: {
                editor_save(ed, dir_cluster, name83);
                ed->modified = 0;
                render(ed, display_name, "running...");
                status = editor_try_run(dir_cluster, name83);
                break;
            }
            default:
                if (ev == '\n') {
                    editor_insert_newline(ed);
                } else if (ev == '\b') {
                    editor_backspace(ed);
                } else if (ev >= 32 && ev < 127) {
                    editor_insert_char(ed, (char)ev);
                }
                break;
        }
    }
}
