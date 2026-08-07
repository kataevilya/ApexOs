#include "process.h"
#include "usermode.h"
#include "serial.h"
#include "elf64.h"
#include "context.h"

__attribute__((noreturn)) extern void halt_forever(void); /* entry.S — запасной путь */

static struct k_jmpbuf g_return_point;
static int g_in_user_program = 0;
static int g_last_exit_code = 0;

int process_is_active(void) { return g_in_user_program; }

int process_run(uint64_t entry, uint64_t user_stack_top) {
    g_in_user_program = 1;

    int rc = k_setjmp(&g_return_point);
    if (rc != 0) {
        /* Сюда мы попали через k_longjmp из process_exit_to_kernel —
           обычный "возврат из функции" здесь никогда не даёт rc!=0
           (k_setjmp сама возвращает 0 при первом вызове). */
        __asm__ volatile ("sti"); /* iretq после этого не будет — сами восстанавливаем IF */
        elf64_unload(); /* освобождаем память программы — иначе повторный run
                            того же адреса упал бы в vmm_map "уже замаплено" */
        return g_last_exit_code;
    }

    enter_usermode(entry, user_stack_top);
    /* недостижимо: enter_usermode либо уходит в ring3, либо (при
       программной ошибке) падает в ud2 -> #UD -> panic(). */
    serial_write("[process] BUG: enter_usermode returned normally\n");
    return -1;
}

void process_exit_to_kernel(int code) {
    if (!g_in_user_program) {
        /* SYS_EXIT вызван не из активной process_run() — не должно
           происходить, но безопаснее остановиться, чем прыгнуть в
           k_jmpbuf с мусором внутри. */
        serial_write("[process] SYS_EXIT with no active process_run() — halting\n");
        halt_forever();
    }
    g_last_exit_code = code;
    g_in_user_program = 0;
    k_longjmp(&g_return_point, 1); /* 1 — просто "вернулись через longjmp", реальный код в g_last_exit_code */
}
