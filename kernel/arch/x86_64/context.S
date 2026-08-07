/*
 * ApexOS — k_setjmp/k_longjmp: минимальный ручной context switch.
 *
 * Это НЕ планировщик и не преемптивная многозадачность — просто способ
 * "запомнить точку в коде ядра" перед входом в ring3 и потом вернуться
 * туда напрямую из обработчика syscall SYS_EXIT, вместо halt_forever().
 * Сохраняются callee-saved регистры (System V AMD64 ABI: rbx, rbp,
 * r12-r15) + rsp + адрес возврата — ровно то же самое, что и в
 * классическом библиотечном setjmp/longjmp.
 */

.section .text
.code64

/* int k_setjmp(struct k_jmpbuf *buf) — buf в rdi.
   Возвращает 0 при обычном вызове; "возвращает" переданное в
   k_longjmp значение, когда управление приходит через неё. */
.global k_setjmp
k_setjmp:
    mov     (%rsp), %rax        /* адрес возврата, положенный инструкцией call */
    mov     %rax, 56(%rdi)      /* buf->rip */
    lea     8(%rsp), %rax       /* rsp вызывающего кода (после этого call) */
    mov     %rax, 48(%rdi)      /* buf->rsp */
    mov     %rbx, 0(%rdi)
    mov     %rbp, 8(%rdi)
    mov     %r12, 16(%rdi)
    mov     %r13, 24(%rdi)
    mov     %r14, 32(%rdi)
    mov     %r15, 40(%rdi)
    xor     %eax, %eax
    ret

/* void k_longjmp(struct k_jmpbuf *buf, int retval) — buf в rdi, retval в esi.
   Не возвращается сюда — управление уходит туда, где был вызван
   k_setjmp, как будто он только что вернул retval. */
.global k_longjmp
k_longjmp:
    mov     %esi, %eax
    mov     0(%rdi), %rbx
    mov     8(%rdi), %rbp
    mov     16(%rdi), %r12
    mov     24(%rdi), %r13
    mov     32(%rdi), %r14
    mov     40(%rdi), %r15
    mov     48(%rdi), %rsp
    jmp     *56(%rdi)

.section .note.GNU-stack, "", @progbits
