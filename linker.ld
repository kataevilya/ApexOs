/*
 * NikiOS linker script
 *
 * Layout:
 *   - .multiboot / .boot.* линкуются по низкому физическому адресу
 *     (1 MiB), т.к. выполняются ДО включения paging и должны быть
 *     физически адресуемы.
 *   - Всё остальное (.text/.rodata/.data/.bss) линкуется в higher half
 *     (KERNEL_VMA), но физически загружается сразу после boot-секции —
 *     этим занимается AT(...), которое GRUB и понимает при загрузке ELF.
 */

ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x00100000;   /* 1 MiB — стандартная точка загрузки для GRUB */

/*
 * Без явного PHDRS линкер по умолчанию схлопывает соседние output-секции
 * в один PT_LOAD и берёт ИЛИ прав всех секций внутри — именно поэтому
 * без этого блока получался один RWX-сегмент на весь .boot и второй
 * RWX на весь .text/.rodata/.bss. Явно разносим по правам: R+X, R+W,
 * R-only отдельно.
 */
PHDRS
{
    boot_text PT_LOAD FLAGS(5);   /* R + X */
    boot_data PT_LOAD FLAGS(6);   /* R + W */
    text      PT_LOAD FLAGS(5);   /* R + X */
    rodata    PT_LOAD FLAGS(4);   /* R only */
    data      PT_LOAD FLAGS(6);   /* R + W */
    bss       PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = KERNEL_LMA;
    _kernel_phys_start = .;

    .boot.text ALIGN(4K) :
    {
        /* Multiboot2 header обязан быть в первых 32 KiB файла —
           поэтому кладём его первым и без зазоров перед ним. */
        KEEP(*(.multiboot2))
        *(.boot.text)
    } :boot_text

    /* Отдельная RW-секция для page tables/стека ранней загрузки —
       не смешиваем с исполняемым кодом .boot.text, чтобы линкер не
       создавал один LOAD-сегмент с RWX-правами сразу на всё. */
    .boot.data ALIGN(4K) :
    {
        *(.boot.data)
    } :boot_data

    . += KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        _text_start = .;
        *(.text)
        *(.text.*)
        _text_end = .;
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
    } :data

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        _bss_end = .;
    } :bss

    _kernel_virt_end = .;
    _kernel_phys_end = . - KERNEL_VMA;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
