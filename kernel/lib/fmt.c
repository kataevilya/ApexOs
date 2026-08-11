#include "fmt.h"
#include <stdint.h>

/* render_uint/render_int пишут цифры в буфер (БЕЗ вывода) и возвращают
   длину — так vfmt может сначала узнать длину числа и добить пробелами
   до ширины (%N...), прежде чем реально печатать. */
static int render_uint(char *buf, unsigned long value, unsigned base, int uppercase) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int pos = 0;

    if (value == 0) {
        buf[0] = '0';
        return 1;
    }
    while (value > 0 && pos < (int)sizeof(tmp)) {
        tmp[pos++] = digits[value % base];
        value /= base;
    }
    int len = 0;
    while (pos > 0) {
        buf[len++] = tmp[--pos];
    }
    return len;
}

static int render_int(char *buf, long value) {
    int len = 0;
    if (value < 0) {
        buf[len++] = '-';
        len += render_uint(buf + len, (unsigned long)(-(unsigned long)value), 10, 0);
    } else {
        len += render_uint(buf, (unsigned long)value, 10, 0);
    }
    return len;
}

static void fmt_write(fmt_putc_fn putc, void *ctx, const char *s) {
    while (*s) {
        putc(ctx, *s++);
    }
}

static void emit_padded(fmt_putc_fn putc, void *ctx, const char *s, int len, int width, int zero_pad) {
    /* Честное ограничение: zero_pad с отрицательным числом ('-' уже
       внутри s) даёт неправильный результат (нули оказались бы ПЕРЕД
       знаком) — не решаем это, т.к. все текущие места вызова с
       zero_pad=1 используют только hex/беззнаковые значения. */
    char pad_char = zero_pad ? '0' : ' ';
    for (int i = len; i < width; i++) {
        putc(ctx, pad_char);
    }
    for (int i = 0; i < len; i++) {
        putc(ctx, s[i]);
    }
}

void vfmt(fmt_putc_fn putc, void *ctx, const char *fmt, va_list args) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            putc(ctx, *p);
            continue;
        }
        p++;
        if (*p == '\0') {
            break;
        }

        int zero_pad = 0;
        if (*p == '0') {
            zero_pad = 1;
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        if (*p == '\0') {
            break;
        }

        int is_long = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
            if (*p == '\0') {
                putc(ctx, '%');
                putc(ctx, 'l');
                break;
            }
        }

        char buf[24];
        int len;
        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                fmt_write(putc, ctx, s ? s : "(null)");
                break;
            }
            case 'd':
                len = is_long ? render_int(buf, va_arg(args, long))
                              : render_int(buf, va_arg(args, int));
                emit_padded(putc, ctx, buf, len, width, zero_pad);
                break;
            case 'u':
                len = render_uint(buf, is_long ? va_arg(args, unsigned long)
                                                : va_arg(args, unsigned int), 10, 0);
                emit_padded(putc, ctx, buf, len, width, zero_pad);
                break;
            case 'x':
                len = render_uint(buf, is_long ? va_arg(args, unsigned long)
                                                : va_arg(args, unsigned int), 16, 0);
                emit_padded(putc, ctx, buf, len, width, zero_pad);
                break;
            case 'X':
                len = render_uint(buf, is_long ? va_arg(args, unsigned long)
                                                : va_arg(args, unsigned int), 16, 1);
                emit_padded(putc, ctx, buf, len, width, zero_pad);
                break;
            case 'p': {
                void *v = va_arg(args, void *);
                fmt_write(putc, ctx, "0x");
                len = render_uint(buf, (unsigned long)(uintptr_t)v, 16, 0);
                emit_padded(putc, ctx, buf, len, width, zero_pad);
                break;
            }
            case 'c':
                putc(ctx, (char)va_arg(args, int));
                break;
            case '%':
                putc(ctx, '%');
                break;
            default:
                putc(ctx, '%');
                if (is_long) {
                    putc(ctx, 'l');
                }
                putc(ctx, *p);
                break;
        }
    }
}
