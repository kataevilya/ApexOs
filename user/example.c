/*
 * Example ApexOS user program — compile with:
 *   make user-app SRC=user/example.c
 * then `make iso` (it becomes the boot module, written to FAT32 as
 * HELLO.ELF automatically) or copy the resulting build/user/app.elf
 * into the FAT32 image some other way once ApexOS has real disk I/O.
 */
#include "apexos_syscall.h"

void _start(void) {
    const char msg[] = "Hello from a real C program compiled against ApexOS!\n";
    sys_write(msg, ni_strlen(msg));
    sys_exit(0);
}
