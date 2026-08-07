/*
 * ApexOS — higher-half entry point
 *
 * Сюда прыгает long_mode_trampoline (boot.S) уже с включённым paging
 * и в реальном 64-битном режиме. Этот код линкуется по KERNEL_VMA
 * (см. linker.ld), поэтому символы здесь — настоящие higher-half адреса.
 */

.section .bss
.align 16
.global kernel_stack_bottom
kernel_stack_bottom:
    .skip 65536         /* 64 KiB начального стека ядра */
.global kernel_stack_top
kernel_stack_top:

.section .text
.code64
.extern kernel_main
.extern mb2_magic_saved
.extern mb2_info_saved
.global higher_half_entry
higher_half_entry:
    /* Переключаемся на higher-half стек. До этого момента стек ещё был
       низкоадресный (boot_stack из boot.S) — он тоже валиден благодаря
       identity-mapping, но дальше ядро должно жить в своей области. */
    movabs  $kernel_stack_top, %rsp
    mov     %rsp, %rbp

    /* Multiboot2 передал magic/info-pointer ещё в 32-битной фазе;
       boot.S сохранил их в mb2_magic_saved / mb2_info_saved (низкие
       физические адреса, но identity-mapping их не трогает). */
    movabs  $mb2_magic_saved, %rax
    mov     (%rax), %edi        /* arg0: magic */
    movabs  $mb2_info_saved, %rax
    mov     (%rax), %esi        /* arg1: multiboot info pointer (физ. адрес) */

    call    kernel_main

    /* kernel_main не должен возвращаться, но на всякий случай — hlt loop,
       а не UB. */
.global halt_forever
halt_forever:
    cli
.hang:
    hlt
    jmp     .hang

.section .note.GNU-stack, "", @progbits
