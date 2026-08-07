/*
 * ApexOS — перезагрузка сегментных регистров после установки новой GDT.
 *
 * В long mode нет прямого "mov %cs, ..." для смены CS — единственный
 * официальный способ перезагрузить CS это far jump/call/ret. Используем
 * классический трюк с lretq: кладём на стек новый CS и адрес возврата,
 * lretq "возвращается" туда же, откуда его позвали, но уже с новым CS.
 */

.section .text
.code64

.global gdt_flush
gdt_flush:
    /* rdi = указатель на struct gdt_pointer */
    lgdt    (%rdi)

    mov     $0x10, %ax      /* GDT_SEL_KERNEL_DATA */
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    pop     %rax            /* адрес возврата, положенный инструкцией call */
    push    $0x08           /* GDT_SEL_KERNEL_CODE */
    push    %rax
    lretq

.global tss_flush
tss_flush:
    mov     $0x28, %ax      /* GDT_SEL_TSS */
    ltr     %ax
    ret

.section .note.GNU-stack, "", @progbits
