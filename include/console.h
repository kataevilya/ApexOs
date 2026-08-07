#ifndef APEXOS_CONSOLE_H
#define APEXOS_CONSOLE_H

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

/* console_init: вычисляет размер сетки символов из текущего разрешения
   framebuffer (fb_width()/fb_height() уже должны быть готовы — вызывать
   после успешного fb_init()). Если framebuffer недоступен — все
   console_* функции становятся no-op (шелл продолжит работать, просто
   без экрана; serial по-прежнему отдельно). */
void console_init(void);

int console_available(void);

void console_putc(char c);

/* Как console_putc, но БЕЗ fb_present() -- для использования внутри
   циклов построчной перерисовки (editor.c::render()), где иначе
   каждый перевод строки означал бы отдельное полное копирование
   кадра. Вызывающий код сам делает один console_present() в конце. */
void console_putc_np(char c);
void console_write(const char *s);

/* Пишет ровно len байт из s (не обязательно NUL-terminated) как
   обычный текст — нужен syscall.c (SYS_WRITE может получить от
   userspace произвольный буфер без гарантии NUL на конце). */
void console_write_len(const char *s, size_t len);
void console_printf(const char *fmt, ...);
void console_vprintf(const char *fmt, va_list args);

/* Пишет ровно len байт из s (не обязательно NUL-terminated — для
   печати подстрок-токенов без лишнего копирования) заданным цветом,
   восстанавливая обычный цвет консоли после. Используется highlight.c;
   для остального кода без явной надобности в цвете — просто шум,
   не используйте напрямую без причины. */
void console_write_len_color(const char *s, size_t len, uint32_t color);

/* Явный "показать кадр на экране" -- нужен после console_write_len_color,
   т.к. она сама больше НЕ делает fb_present() (см. её комментарий в
   console.c про причину). Вызывать один раз в конце пакета вывода,
   не после каждого токена/символа. */
void console_present(void);

/* Рисует курсор (тонкая полоса снизу ячейки) в позиции (col,row) —
   используется полноэкранным редактором. col/row вне текущей сетки
   символов — тихий no-op, как и остальные примитивы рисования. */
void console_draw_cursor(uint32_t col, uint32_t row, uint32_t color);

/* Размер сетки символов текущего режима — редактору нужно знать,
   сколько строк/столбцов реально помещается на экране. Оба возвращают
   0, если консоль недоступна. */
uint32_t console_cols(void);
uint32_t console_rows(void);

/* Обрабатывает Backspace: стирает последний введённый на экране символ
   (используется shell'ом при редактировании строки ввода). */
void console_backspace(void);

void console_clear(void);

#endif /* APEXOS_CONSOLE_H */
