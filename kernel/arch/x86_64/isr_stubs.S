/*
 * ApexOS — низкоуровневые заглушки прерываний.
 *
 * Векторы 0-31 — исключения CPU. Часть из них (8,10,11,12,13,14,17)
 * аппаратно кладут на стек error code, остальные — нет; для единого
 * layout стека мы вручную кладём error_code=0 там, где CPU его не даёт.
 * Векторы 32-255 (IRQ после remap PIC + будущие software-прерывания)
 * error code никогда не получают — тоже кладём 0.
 *
 * Каждый isrN кладёт [error_code][vector] и прыгает в общий
 * isr_common_stub, который сохраняет регистры, вызывает isr_handler(C),
 * восстанавливает регистры и делает iretq.
 */

.altmacro

.macro isr_noerr num
.section .text
.code64
.global isr\num
isr\num:
    cli
    pushq   $0
    pushq   $\num
    jmp     isr_common_stub
.endm

.macro isr_err num
.section .text
.code64
.global isr\num
isr\num:
    cli
    pushq   $\num
    jmp     isr_common_stub
.endm

/* --- CPU exceptions (0-31) --- */
isr_noerr 0
isr_noerr 1
isr_noerr 2
isr_noerr 3
isr_noerr 4
isr_noerr 5
isr_noerr 6
isr_noerr 7
isr_err   8
isr_noerr 9
isr_err   10
isr_err   11
isr_err   12
isr_err   13
isr_err   14
isr_noerr 15
isr_noerr 16
isr_err   17
isr_noerr 18
isr_noerr 19
isr_noerr 20
isr_noerr 21
isr_noerr 22
isr_noerr 23
isr_noerr 24
isr_noerr 25
isr_noerr 26
isr_noerr 27
isr_noerr 28
isr_noerr 29
isr_noerr 30
isr_noerr 31

/* --- IRQ / software interrupts (32-255) --- */
.set i, 32
.rept (256 - 32)
    isr_noerr %i
    .set i, i+1
.endr

/*
 * isr_common_stub: сохраняет регистры в порядке, который ожидает
 * struct registers (см. idt.h), вызывает isr_handler(rsp) и
 * восстанавливает состояние. Порядок push/pop ЗЕРКАЛЕН друг другу.
 */
.section .text
.code64
.extern isr_handler
isr_common_stub:
    push    %rax
    push    %rbx
    push    %rcx
    push    %rdx
    push    %rsi
    push    %rdi
    push    %rbp
    push    %r8
    push    %r9
    push    %r10
    push    %r11
    push    %r12
    push    %r13
    push    %r14
    push    %r15

    mov     %rsp, %rdi      /* arg0 = &struct registers (текущий rsp) */
    call    isr_handler

    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rbp
    pop     %rdi
    pop     %rsi
    pop     %rdx
    pop     %rcx
    pop     %rbx
    pop     %rax

    add     $16, %rsp       /* убрать error_code + vector */
    /* IF восстанавливается автоматически из сохранённых RFLAGS —
       явный sti здесь не нужен и был бы избыточен/вводил бы в
       заблуждение о том, что реально включает прерывания. */
    iretq

/*
 * isr_stub_table: массив адресов isr0..isr255 — заполняется идентичным
 * altmacro-циклом, чтобы idt_init() (C) мог сделать
 * idt_set_gate(i, isr_stub_table[i], ...) без 256 extern-объявлений.
 */
.macro isr_table_entry num
    .quad isr\num
.endm

.section .data
.align 8
.global isr_stub_table
isr_stub_table:
.set i, 0
.rept 256
    isr_table_entry %i
    .set i, i+1
.endr

.section .note.GNU-stack, "", @progbits
