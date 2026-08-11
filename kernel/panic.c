#include "panic.h"
#include "serial.h"
#include "console.h"
#include "fb.h"
#include <stdarg.h>

__attribute__((noreturn)) extern void halt_forever(void); /* kernel/arch/x86_64/entry.S */

/*
 * panic() must be visible WITHOUT a serial cable -- on real hardware
 * (no way to see the serial log) a kernel that only logs fatal errors
 * to a port nobody's listening to is effectively silent about its own
 * death. console_write/console_vprintf are safe to call even before
 * console_init() has run (they no-op if the console isn't ready yet),
 * so this is safe to call from anywhere, including very early boot.
 */
void panic(const char *fmt, ...) {
    va_list args1, args2;

    serial_write("\n[KERNEL PANIC] ");
    console_write_len_color("\n[KERNEL PANIC] ", 9, fb_make_color(0xFF, 0x40, 0x40));

    va_start(args1, fmt);
    va_copy(args2, args1);
    serial_vprintf(fmt, args1);
    console_vprintf(fmt, args2);
    va_end(args1);
    va_end(args2);

    serial_write("\n");
    console_write("\nSystem halted.\n");
    halt_forever();
}
