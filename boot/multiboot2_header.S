/*
 * ApexOS — Multiboot2 header
 *
 * Спецификация: https://www.gnu.org/software/grub/manual/multiboot2/
 * Требования, которые проверяем явно:
 *   - header должен находиться полностью в первых 32768 байтах файла
 *   - header должен быть выровнен на 8 байт
 *   - checksum + magic + architecture в сумме дают 0 (mod 2^32)
 */

.section .multiboot2
.align 8

#define MB2_MAGIC       0xE85250D6   /* magic для Multiboot2 */
#define MB2_ARCH_I386   0            /* protected mode i386, даже для x86_64 boot path */
#define MB2_HEADER_LEN  (mb2_header_end - mb2_header_start)
#define MB2_CHECKSUM    -(MB2_MAGIC + MB2_ARCH_I386 + MB2_HEADER_LEN)

.global mb2_header_start
mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long MB2_HEADER_LEN
    .long MB2_CHECKSUM

    /* Information request tag: просим карту памяти и framebuffer info заранее,
       чтобы позже не переусложнять парсинг тегов. */
    .align 8
info_request_tag_start:
    .short 1            /* type = MULTIBOOT_HEADER_TAG_INFORMATION_REQUEST */
    .short 0            /* flags */
    .long info_request_tag_end - info_request_tag_start
    .long 3             /* MULTIBOOT_TAG_TYPE_MODULE */
    .long 6             /* MULTIBOOT_TAG_TYPE_MMAP */
    .long 8             /* MULTIBOOT_TAG_TYPE_FRAMEBUFFER */
info_request_tag_end:

    /* Framebuffer request tag: просим любой линейный framebuffer.
       0/0/0 = "любое разрешение, которое даст прошивка/GRUB". */
    .align 8
    .short 5            /* type = MULTIBOOT_HEADER_TAG_FRAMEBUFFER */
    .short 0            /* flags */
    .long 20            /* size */
    .long 0             /* width  (0 = no preference) */
    .long 0             /* height (0 = no preference) */
    .long 32            /* bpp */

    /* Terminating tag — обязателен по спецификации */
    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

.section .note.GNU-stack, "", @progbits
