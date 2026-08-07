/*
 * Interactive ApexOS example: echoes typed characters back until Enter.
 *   make user-app SRC=user/echo_example.c
 */
#include "apexos_syscall.h"

void _start(void) {
    const char prompt[] = "Type something, Enter to finish:\n";
    sys_write(prompt, ni_strlen(prompt));

    char c;
    while (1) {
        if (sys_read_char(&c) != 1) {
            continue;
        }
        sys_write(&c, 1);
        if (c == '\n') {
            break;
        }
    }

    const char bye[] = "Got it -- exiting.\n";
    sys_write(bye, ni_strlen(bye));
    sys_exit(0);
}
