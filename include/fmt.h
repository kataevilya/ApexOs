#ifndef APEXOS_FMT_H
#define APEXOS_FMT_H

#include <stdarg.h>

typedef void (*fmt_putc_fn)(void *ctx, char c);

/* vfmt: общий движок форматирования для serial_vprintf и console_printf —
   одна реализация %s/%d/%u/%x/%p/%c/%%/%lx/%lu/%ld, не дублированная в
   двух местах (что раньше было бы источником расхождения при правке
   одного и забытом втором). putc(ctx, c) вызывается на каждый выходной
   символ; ctx непрозрачен для vfmt. */
void vfmt(fmt_putc_fn putc, void *ctx, const char *fmt, va_list args);

#endif /* APEXOS_FMT_H */
