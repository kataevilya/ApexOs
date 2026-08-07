/*
 * ApexOS — 32-bit boot trampoline
 *
 * GRUB передаёт управление сюда в 32-bit protected mode.
 * Задачи этого файла:
 *   1. Проверить, что нас загрузил именно Multiboot2-совместимый загрузчик.
 *   2. Проверить наличие CPUID и long mode (иначе ядро не запустится — hlt с кодом ошибки).
 *   3. Построить начальные page tables: identity-map первых 64 MiB
 *      и те же 64 MiB — в higher-half (KERNEL_VMA).
 *   4. Включить PAE, long mode (EFER.LME), paging.
 *   5. Через far jump с новым 64-бит GDT перейти в 64-bit код (ещё по identity-адресу),
 *      а оттуда — прыгнуть на настоящий higher-half entry point.
 *
 * ВАЖНО: до включения paging весь код должен быть физически адресуем,
 * поэтому эта секция (.boot) линкуется по НИЗКОМУ адресу (см. linker.ld).
 */

/* Смещения селекторов в gdt64 (null=0, code=8, data=16) — фиксированы
   структурой таблицы ниже. Определены здесь, ДО первого использования
   в _start, потому что #define в GAS раскрывается препроцессором
   линейно по тексту файла (как в C), в отличие от меток, которые
   резолвит уже ассемблер. */
#define GDT64_CODE_SEG 0x08
#define GDT64_DATA_SEG 0x10

.section .boot.data, "aw"
.align 16

/* Небольшой стек для 32-бит фазы загрузки. Он в идентити-регионе,
   поэтому доступен и до, и после включения paging. */
boot_stack_bottom:
    .skip 4096
boot_stack_top:

/* Сюда сохраняем то, что передал GRUB, чтобы отдать в kernel_main
   уже в 64-битном режиме. */
.global mb2_magic_saved
mb2_magic_saved:
    .long 0
.global mb2_info_saved
mb2_info_saved:
    .long 0

.align 4096
.global boot_pml4
boot_pml4:
    .skip 4096
boot_pdpt_low:
    .skip 4096
boot_pdpt_high:
    .skip 4096
boot_pd_low:
    .skip 4096
boot_pd_high:
    .skip 4096

.section .boot.text, "ax"
.code32

.global _start
.type _start, @function
_start:
    cli
    mov     $boot_stack_top, %esp
    mov     %esp, %ebp

    /* GRUB отдаёт: eax = 0x36d76289 (magic), ebx = физ. адрес Multiboot2 info */
    mov     %eax, mb2_magic_saved
    mov     %ebx, mb2_info_saved

    call    check_multiboot2
    call    check_cpuid
    call    check_long_mode
    call    setup_page_tables
    call    enable_paging

    lgdt    gdt64_pointer

    /* Далёкий переход перезагружает CS новым 64-битным селектором —
       именно в этот момент CPU реально переходит в long mode. */
    ljmp    $GDT64_CODE_SEG, $long_mode_trampoline

    /* Сюда попасть не должны */
    hlt

/*
 * check_multiboot2: eax должен содержать magic 0x36d76289.
 * Если нет — нас загрузил не Multiboot2-загрузчик, дальше делать нечего.
 */
check_multiboot2:
    cmp     $0x36d76289, %eax
    jne     .no_multiboot
    ret
.no_multiboot:
    mov     $'M', %al
    jmp     boot_error

/*
 * check_cpuid: пытаемся перевернуть бит 21 (ID) в EFLAGS.
 * Если после этого бит остался прежним — CPUID не поддерживается
 * (на реальном x86_64 такое практически невозможно, но проверяем честно).
 */
check_cpuid:
    pushfl
    pop     %eax
    mov     %eax, %ecx
    xor     $(1 << 21), %eax
    push    %eax
    popfl
    pushfl
    pop     %eax
    push    %ecx
    popfl
    cmp     %ecx, %eax
    je      .no_cpuid
    ret
.no_cpuid:
    mov     $'C', %al
    jmp     boot_error

/*
 * check_long_mode: через CPUID проверяем сначала, что расширенные функции
 * (0x80000000+) доступны, потом бит 29 (LM) в EDX функции 0x80000001.
 */
check_long_mode:
    mov     $0x80000000, %eax
    cpuid
    cmp     $0x80000001, %eax
    jb      .no_long_mode

    mov     $0x80000001, %eax
    cpuid
    test    $(1 << 29), %edx
    jz      .no_long_mode
    ret
.no_long_mode:
    mov     $'L', %al
    jmp     boot_error

/*
 * setup_page_tables: строим минимальные таблицы через 2MiB huge pages
 * (PDE.PS=1), чтобы не возиться с уровнем PT на этом этапе.
 * Отображаем 64 MiB (32 записи по 2 MiB) в двух местах:
 *   - identity: 0x0000000000000000
 *   - higher half: KERNEL_VMA (0xFFFFFFFF80000000), PML4[511]/PDPT[510]
 */
#include "mm_layout.h"

#define PAGE_PRESENT   (1 << 0)
#define PAGE_WRITABLE  (1 << 1)
#define PAGE_HUGE      (1 << 7)
#define NUM_PD_ENTRIES APEXOS_STATIC_MAP_HUGE_PAGES   /* см. include/mm_layout.h */

setup_page_tables:
    /* PML4[0] -> boot_pdpt_low */
    mov     $boot_pdpt_low, %eax
    or      $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov     $boot_pml4, %edi
    mov     %eax, (%edi)

    /* PML4[511] -> boot_pdpt_high */
    mov     $boot_pdpt_high, %eax
    or      $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov     $boot_pml4, %edi
    mov     %eax, 511 * 8(%edi)

    /* PDPT_low[0] -> boot_pd_low */
    mov     $boot_pd_low, %eax
    or      $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov     $boot_pdpt_low, %edi
    mov     %eax, (%edi)

    /* PDPT_high[510] -> boot_pd_high */
    mov     $boot_pd_high, %eax
    or      $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    mov     $boot_pdpt_high, %edi
    mov     %eax, 510 * 8(%edi)

    /* Заполняем PD_low и PD_high одинаковыми huge-page записями:
       entry[i] = (i * 2MiB) | PRESENT | WRITABLE | HUGE */
    mov     $0, %ecx
.fill_pd_loop:
    mov     %ecx, %eax
    shl     $21, %eax           /* eax = i * 2MiB (физический адрес) */
    or      $(PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE), %eax

    mov     $boot_pd_low, %edi
    mov     %eax, (%edi, %ecx, 8)

    mov     $boot_pd_high, %edi
    mov     %eax, (%edi, %ecx, 8)

    inc     %ecx
    cmp     $NUM_PD_ENTRIES, %ecx
    jl      .fill_pd_loop

    ret

/*
 * enable_paging:
 *   1. CR3 = physical address of PML4
 *   2. CR4.PAE = 1 (без PAE 64-битные page tables недоступны)
 *   3. EFER.LME = 1 (long mode enable, через MSR 0xC0000080)
 *   4. CR0.PG = 1 (наконец включаем paging — теперь мы в compatibility mode)
 */
enable_paging:
    mov     $boot_pml4, %eax
    mov     %eax, %cr3

    mov     %cr4, %eax
    or      $(1 << 5), %eax     /* CR4.PAE */
    mov     %eax, %cr4

    mov     $0xC0000080, %ecx  /* IA32_EFER */
    rdmsr
    or      $(1 << 8), %eax    /* EFER.LME */
    wrmsr

    mov     %cr0, %eax
    or      $(1 << 31), %eax   /* CR0.PG */
    mov     %eax, %cr0

    ret

/*
 * boot_error: примитивный вывод "ERR: <код>" прямо в VGA text buffer
 * (0xB8000), потому что serial ещё не инициализирован на этой стадии.
 * Используется только для фатальных ошибок раннего boot, до появления
 * полноценного serial-логирования в C-коде.
 */
boot_error:
    mov     $0xB8000, %edx
    movb    $'E', (%edx)
    movb    $0x4F, 1(%edx)
    movb    $'R', 2(%edx)
    movb    $0x4F, 3(%edx)
    movb    $'R', 4(%edx)
    movb    $0x4F, 5(%edx)
    movb    $':', 6(%edx)
    movb    $0x4F, 7(%edx)
    movb    %al, 8(%edx)
    movb    $0x4F, 9(%edx)
.hang:
    hlt
    jmp     .hang

/* ------------------------------------------------------------------ */
/* Временный 64-битный GDT — нужен только чтобы far jump переключил CS */
/* в 64-битный код segment. Полноценный GDT/TSS ядро строит позже,    */
/* в kernel/arch/x86_64/gdt.c.                                         */
/* ------------------------------------------------------------------ */
.section .boot.data, "aw"
.align 16
gdt64_start:
    .quad 0x0000000000000000                  /* null descriptor */
gdt64_code:
    .quad 0x00AF9A000000FFFF                   /* 64-bit code: L=1,P=1,DPL0,exec/read */
gdt64_data:
    .quad 0x00AF92000000FFFF                   /* 64-bit data: P=1,DPL0,read/write */
gdt64_end:

gdt64_pointer:
    .word gdt64_end - gdt64_start - 1
    .quad gdt64_start

/* ------------------------------------------------------------------ */
/* long_mode_trampoline: первые инструкции, которые CPU выполняет уже */
/* в реальном 64-битном режиме. Мы всё ещё выполняемся по identity-   */
/* адресу (эта секция линкуется по низкому физическому адресу), но    */
/* обе таблицы (identity и higher-half) уже валидны, так что можно    */
/* безопасно прыгнуть на символ из .text, который линкован по         */
/* высокому (higher-half) виртуальному адресу.                        */
/* ------------------------------------------------------------------ */
.section .boot.text, "ax"
.code64
.global long_mode_trampoline
long_mode_trampoline:
    mov     $GDT64_DATA_SEG, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Абсолютный прыжок на higher-half виртуальный адрес — после этого
       мы окончательно "переезжаем" в верхнюю половину адресного
       пространства и можем безопасно продолжать в entry.S (kernel/). */
    movabs  $higher_half_entry, %rax
    jmp     *%rax

.section .note.GNU-stack, "", @progbits
