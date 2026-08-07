/*
 * ApexOS — тестовая userspace-программа.
 *
 * Полностью независима от ядра: нет libc, нет общих заголовков — только
 * прямой int 0x80 по соглашению ApexOS (rax=номер syscall, rdi/rsi/rdx=
 * аргументы). Компилируется отдельно (user/user.ld, ET_EXEC, фиксированный
 * адрес 0x400000) и передаётся ядру как Multiboot2-модуль — файловой
 * системы, с которой можно было бы прочитать реальный ELF с диска, у нас
 * пока нет, поэтому это единственный честный способ получить настоящий
 * ELF64-файл в память ядра на этом этапе (см. пункт про модули в main.c).
 */

.section .text
.global _start
_start:
    /* SYS_WRITE(buf, len) — fd пока не используется (некуда писать,
       кроме serial), сигнатура зарезервирована под будущий VFS. */
    mov     $msg, %rdi
    mov     $msg_len, %rsi
    mov     $1, %rax        /* SYS_WRITE */
    int     $0x80

    /* SYS_EXIT(code) */
    mov     $0, %rdi
    mov     $0, %rax        /* SYS_EXIT */
    int     $0x80

    /* Недостижимо при исправном ядре (SYS_EXIT не возвращается) —
       но на случай регрессии не крутим пустой busy-loop, а именно hlt. */
.hang:
    hlt
    jmp     .hang

.section .rodata
msg:
    .ascii "Hello from a real ring3 ELF64 process (ApexOS milestone 6)!\n"
msg_len = . - msg

.section .note.GNU-stack, "", @progbits
