/*
 * ApexOS — переход в ring 3.
 *
 * iretq — единственный официальный способ понизить привилегии CPU в
 * long mode. Строим на стеке кадр, который CPU ожидает: SS, RSP,
 * RFLAGS, CS, RIP (в этом порядке снизу вверх стека перед iretq).
 */

.section .text
.code64

.global enter_usermode
enter_usermode:
    /* rdi = entry point (виртуальный адрес в userspace)
       rsi = вершина пользовательского стека */

    mov     $0x23, %ax      /* GDT_SEL_USER_DATA_RPL3 = 0x20 | 3 */
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    /* SS не трогаем через mov — его поставит сам iretq из значения,
       которое мы сейчас положим на стек ниже. */

    push    $0x23           /* SS (RPL3, иначе iretq считает переход некорректным) */
    push    %rsi            /* RSP пользователя */

    pushfq
    pop     %rax
    or      $0x200, %rax    /* принудительно IF=1 — иначе ring3-код стартует с выключенными прерываниями */
    push    %rax            /* RFLAGS */

    push    $0x1B           /* GDT_SEL_USER_CODE_RPL3 = 0x18 | 3 */
    push    %rdi            /* RIP: точка входа пользовательской программы */

    iretq
    /* Сюда управление не возвращается: CPU теперь в ring3. Если всё же
       вернулось — это фатальная ошибка (плохой selector/RIP), а не
       что-то, что можно тихо продолжить. */
    ud2

.section .note.GNU-stack, "", @progbits
