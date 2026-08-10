#include <stdint.h>
#include <stddef.h>
#include "serial.h"
#include "multiboot2.h"
#include "panic.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "string.h"
#include "fb.h"
#include "console.h"
#include "elf64.h"
#include "syscall.h"
#include "usermode.h"
#include "ramdisk.h"
#include "fat32.h"
#include "net.h"
#include "apxp.h"
#include "shell.h"
#include "highlight.h"
#include "mm_layout.h"

/*
 * parse_multiboot_tags: проходим по тегам Multiboot2 info-структуры,
 * НЕ доверяя данным от загрузчика вслепую:
 *   - info_phys обязан быть не NULL и выровнен на 8 байт (требование спеки);
 *   - total_size проверяется на разумные границы;
 *   - каждый тег читается только если целиком помещается до конца
 *     объявленного total_size (иначе — не читаем, чтобы не уйти за
 *     пределы структуры, переданной GRUB).
 *
 * Отдаёт указатели на mmap/framebuffer теги через out-параметры (NULL,
 * если тег отсутствует или не прошёл проверки границ). Модули (может
 * быть НЕСКОЛЬКО — раньше здесь брался только первый, отсюда жалоба
 * "видит только hello.elf") отдаются через callback on_module,
 * вызываемый для КАЖДОГО найденного module-тега. out_total_size
 * получает total_size всей info-структуры (нужен pmm_init).
 */
typedef void (*module_tag_cb)(const struct multiboot_tag_module *tag, void *ctx);

static void parse_multiboot_tags(uint32_t info_phys,
                                  const struct multiboot_tag_mmap **out_mmap_tag,
                                  const struct multiboot_tag_framebuffer **out_fb_tag,
                                  module_tag_cb on_module, void *module_ctx,
                                  uint32_t *out_total_size) {
    *out_mmap_tag = NULL;
    *out_fb_tag = NULL;

    if (info_phys == 0) {
        panic("multiboot2 info pointer is NULL");
    }
    if (info_phys % 8 != 0) {
        panic("multiboot2 info pointer 0x%x is not 8-byte aligned", info_phys);
    }

    const struct multiboot_info_header *hdr =
        (const struct multiboot_info_header *)(uintptr_t)info_phys;

    uint32_t total_size = hdr->total_size;
    *out_total_size = total_size;

    /* Разумный верхний предел: реальная multiboot2-структура не бывает
       гигантской. 16 MiB — с большим запасом, но отсекает явно битые
       значения (например, если total_size прочитан из мусора). */
    if (total_size < sizeof(struct multiboot_info_header) || total_size > (16u * 1024 * 1024)) {
        panic("multiboot2 total_size looks invalid: %u", total_size);
    }

    uint32_t offset = sizeof(struct multiboot_info_header);
    int module_count = 0;

    while (offset + sizeof(struct multiboot_tag) <= total_size) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)((const uint8_t *)hdr + offset);

        /* Тег не должен заявлять размер, выходящий за total_size —
           иначе дальнейшее чтение его данных читало бы за пределами
           структуры, которую реально предоставил загрузчик. */
        if (tag->size < sizeof(struct multiboot_tag) || offset + tag->size > total_size) {
            panic("multiboot2 tag at offset %u has invalid size %u", offset, tag->size);
        }

        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            break;
        }

        switch (tag->type) {
            case MULTIBOOT_TAG_TYPE_MMAP:
                serial_printf("[mb2] memory map tag found (size=%u)\n", tag->size);
                *out_mmap_tag = (const struct multiboot_tag_mmap *)tag;
                break;
            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
                serial_printf("[mb2] framebuffer tag found (size=%u)\n", tag->size);
                *out_fb_tag = (const struct multiboot_tag_framebuffer *)tag;
                break;
            case MULTIBOOT_TAG_TYPE_MODULE:
                serial_printf("[mb2] module tag found (size=%u)\n", tag->size);
                module_count++;
                if (on_module != NULL) {
                    on_module((const struct multiboot_tag_module *)tag, module_ctx);
                }
                break;
            default:
                /* Неизвестные/неинтересные пока теги просто пропускаем. */
                break;
        }

        /* Теги выровнены на 8 байт — продвигаемся с учётом паддинга. */
        offset += (tag->size + 7u) & ~7u;
    }

    if (*out_mmap_tag == NULL) {
        serial_write("[mb2] WARNING: no memory map tag provided by bootloader\n");
    }
    if (*out_fb_tag == NULL) {
        serial_write("[mb2] WARNING: no framebuffer tag provided by bootloader\n");
    }
    if (module_count == 0) {
        serial_write("[mb2] WARNING: no module tags provided by bootloader\n");
    } else {
        serial_printf("[mb2] %d module tag(s) found\n", module_count);
    }
}

struct module_range {
    uint32_t min_start;
    uint32_t max_end;
    int count;
};

static void module_range_cb(const struct multiboot_tag_module *tag, void *ctx_) {
    struct module_range *ctx = (struct module_range *)ctx_;
    if (tag->mod_end < tag->mod_start) {
        return; /* битый тег — обработаем (и пожалуемся) позже, при реальной записи в FAT32 */
    }
    if (ctx->count == 0 || tag->mod_start < ctx->min_start) {
        ctx->min_start = tag->mod_start;
    }
    if (ctx->count == 0 || tag->mod_end > ctx->max_end) {
        ctx->max_end = tag->mod_end;
    }
    ctx->count++;
}

struct module_write_state {
    int index; /* следующий порядковый номер для generic-имени, если cmdline не годится */
};

/* make_generic_name: "MODn" (короткое 8.3-имя без расширения) — на
   случай, если cmdline модуля отсутствует/не 8.3-совместимо. Без
   snprintf (его у нас нет) — вручную, но только для однозначных n
   в разумных пределах (десятки модулей более чем достаточно). */
static void make_generic_name(int n, char out83[FAT32_NAME_LEN]) {
    char name[8];
    name[0] = 'M'; name[1] = 'O'; name[2] = 'D';
    int pos = 3;
    char digits[6];
    int dn = 0;
    if (n < 0) n = 0;
    if (n == 0) {
        digits[dn++] = '0';
    } else {
        while (n > 0 && dn < (int)sizeof(digits)) {
            digits[dn++] = (char)('0' + n % 10);
            n /= 10;
        }
    }
    while (dn > 0 && pos < (int)sizeof(name) - 1) {
        name[pos++] = digits[--dn];
    }
    name[pos] = '\0';
    fat32_name_to_83(name, out83); /* всегда валидно по построению (буквы+цифры, <=8 символов) */
}

static void module_write_cb(const struct multiboot_tag_module *tag, void *ctx_) {
    struct module_write_state *st = (struct module_write_state *)ctx_;
    int this_index = st->index++;

    if (tag->mod_end < tag->mod_start) {
        serial_write("[boot] WARNING: a module tag has mod_end < mod_start, skipping it\n");
        return;
    }

    size_t cmdline_offset = offsetof(struct multiboot_tag_module, cmdline);
    size_t cmdline_max = (tag->size > cmdline_offset) ? tag->size - cmdline_offset : 0;
    int cmdline_terminated = 0;
    for (size_t i = 0; i < cmdline_max; i++) {
        if (tag->cmdline[i] == '\0') { cmdline_terminated = 1; break; }
    }

    char name83[FAT32_NAME_LEN];
    int have_name = 0;
    if (cmdline_terminated && tag->cmdline[0] != '\0') {
        if (fat32_name_to_83(tag->cmdline, name83) == 0) {
            have_name = 1;
        }
    }
    if (!have_name) {
        make_generic_name(this_index, name83);
    }

    char display_name[13];
    fat32_83_to_display((const uint8_t *)name83, display_name);
    serial_printf("[boot] module #%d: phys=[0x%x, 0x%x), name=%s%s\n",
                  this_index, tag->mod_start, tag->mod_end, display_name,
                  have_name ? "" : " (generic -- cmdline missing/not 8.3-compatible)");

    /* НЕ предполагаем, что модуль обязательно лежит внутри статической
       identity-map (спецификация Multiboot2 этого не гарантирует) —
       если он вне этого диапазона, честно мапим его физические
       страницы через VMM во временный virtual диапазон перед чтением. */
    uint64_t mod_phys_start = tag->mod_start;
    uint64_t mod_phys_end = tag->mod_end;
    size_t mod_size = (size_t)(mod_phys_end - mod_phys_start);
    const void *mod_ptr;

    if (mod_phys_end <= APEXOS_STATIC_MAP_BYTES) {
        mod_ptr = (const void *)(uintptr_t)mod_phys_start;
    } else {
        uint64_t page_start = mod_phys_start & ~(uint64_t)(VMM_PAGE_SIZE - 1);
        uint64_t offset_in_page = mod_phys_start - page_start;
        uint64_t total_bytes = offset_in_page + mod_size;
        uint64_t pages = (total_bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
        /* Отдельный диапазон на модуль, чтобы несколько модулей не
           перекрывались друг с другом при временном маппинге. */
        uint64_t temp_virt_base = 0xFFFFFFFFF0000000ull + (uint64_t)this_index * 0x10000000ull;
        for (uint64_t i = 0; i < pages; i++) {
            vmm_map(temp_virt_base + i * VMM_PAGE_SIZE, page_start + i * VMM_PAGE_SIZE,
                    VMM_PRESENT | VMM_WRITABLE);
        }
        mod_ptr = (const void *)(uintptr_t)(temp_virt_base + offset_in_page);
        serial_write("[boot] module was outside the static map -- mapped separately via VMM\n");
    }

    if (fat32_write_file(fat32_root_cluster(), name83, mod_ptr, (uint32_t)mod_size) != 0) {
        serial_printf("[boot] WARNING: failed writing module #%d to FAT32\n", this_index);
    } else {
        serial_printf("[boot] wrote module #%d (%s) to FAT32\n", this_index, display_name);
    }
}

void kernel_main(uint32_t magic, uint32_t mb_info_phys) {
    /* serial_init() должен быть первым вызовом — весь дальнейший лог
       (включая panic()) идёт через него. Если реального UART нет,
       serial_* становятся no-op, но код продолжает работать корректно. */
    serial_init();

    serial_write("\n=== ApexOS boot ===\n");
    serial_printf("[boot] multiboot2 magic = 0x%x\n", magic);
    serial_printf("[boot] multiboot2 info  = 0x%x\n", mb_info_phys);

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        panic("invalid multiboot2 magic: 0x%x (expected 0x%x)",
              magic, MULTIBOOT2_BOOTLOADER_MAGIC);
    }

    const struct multiboot_tag_mmap *mmap_tag = NULL;
    const struct multiboot_tag_framebuffer *fb_tag = NULL;
    uint32_t mb_info_total_size = 0;
    struct module_range mod_range = { 0, 0, 0 };

    parse_multiboot_tags(mb_info_phys, &mmap_tag, &fb_tag, module_range_cb, &mod_range, &mb_info_total_size);

    serial_write("[boot] long mode + higher-half mapping OK\n");

    /*
     * Milestone 2: прерывания.
     *   1. Настоящая GDT+TSS (заменяет временную из boot.S).
     *   2. IDT на 256 векторов, обработчики CPU-исключений.
     *   3. Remap 8259 PIC (иначе IRQ0-7 конфликтуют с исключениями CPU).
     *   4. PIT (IRQ0) как источник системного таймера.
     *   5. sti — включаем прерывания.
     *   6. Self-test: int3 (breakpoint) должен пройти полный цикл
     *      isr_common_stub -> isr_handler -> iretq и вернуть управление
     *      следующей инструкции — если это не так, дальше печатать
     *      "self-test passed" просто не получится.
     */
    gdt_init();
    idt_init();
    syscall_init(); /* поднимает DPL вектора 0x80 до 3 — без этого int 0x80 из ring3 упал бы в #GP */

    pic_remap(PIC1_VECTOR_OFFSET, PIC2_VECTOR_OFFSET);
    pic_mask_all(); /* сначала маскируем всё, размаскируем явно по готовности */

    pit_init(100); /* 100 Гц системный таймер */
    keyboard_init();

    __asm__ volatile ("sti");
    pic_unmask_irq(0); /* таймер — только после sti */
    pic_unmask_irq(1); /* клавиатура — только после sti */

    serial_write("[boot] interrupts enabled (IDT + PIC remap + PIT @ 100Hz)\n");

    __asm__ volatile ("int $3");
    serial_write("[boot] int3 self-test passed: execution resumed after ISR\n");

    /* Ждём несколько тиков таймера — если PIT/PIC/IDT реально работают
       end-to-end, g_pit_ticks (через pit_get_ticks()) будет расти без
       нашего участия, только за счёт аппаратных прерываний. */
    uint64_t target = pit_get_ticks() + 5;
    while (pit_get_ticks() < target) {
        __asm__ volatile ("hlt"); /* спим до следующего прерывания, не жжём CPU busy-loop'ом */
    }
    serial_printf("[boot] PIT self-test passed: %lu ticks observed\n",
                  (unsigned long)pit_get_ticks());

    /*
     * Клавиатурный self-test: echo всего, что придёт с PS/2, в serial —
     * но ОГРАНИЧЕННОЕ время (10 секунд при 100 Гц = 1000 тиков), а не
     * бесконечное ожидание нажатия клавиши. Если ввода не было — это
     * не ошибка (в QEMU без интерактивного stdin ввода и не будет),
     * просто тест завершается по таймауту, а не висит вечно.
     */
    serial_write("[boot] keyboard echo self-test: type on PS/2 keyboard for 10s (or nothing)\n");
    uint64_t kb_test_deadline = pit_get_ticks() + 1000;
    int kb_chars_seen = 0;
    while (pit_get_ticks() < kb_test_deadline) {
        char c;
        if (keyboard_read_char(&c)) {
            serial_printf("[kbd] got char: '%c' (0x%x)\n", c, (unsigned)c);
            kb_chars_seen++;
        } else {
            __asm__ volatile ("hlt");
        }
    }
    serial_printf("[boot] keyboard self-test done: %d char(s) seen\n", kb_chars_seen);

    /*
     * Milestone 3: физическая память.
     *   pmm_init строит битовую карту на основе Multiboot2 memory map,
     *   резервируя нулевую страницу, физический образ ядра и саму
     *   info-структуру. Без mmap-тега продолжать нельзя — без него
     *   неизвестно, какая физическая память вообще безопасна для
     *   аллокатора, поэтому это panic(), а не "работаем как есть".
     */
    if (mmap_tag == NULL) {
        panic("cannot initialize PMM: no multiboot2 memory map tag");
    }
    pmm_init(mmap_tag, mb_info_phys, mb_info_total_size,
             mod_range.count > 0 ? mod_range.min_start : 0,
             mod_range.count > 0 ? mod_range.max_end : 0);

    /* Self-test: выделяем несколько фреймов, проверяем что они разные
       и выровнены, освобождаем их обратно, проверяем что free_frames
       вернулся к исходному значению — иначе аллокатор либо теряет
       память (leak), либо считает её дважды. */
    uint64_t free_before = pmm_free_frames();
    uint64_t frame_a = pmm_alloc_frame();
    uint64_t frame_b = pmm_alloc_frame();
    uint64_t frame_c = pmm_alloc_frame();

    if (frame_a == 0 || frame_b == 0 || frame_c == 0) {
        panic("pmm self-test: pmm_alloc_frame returned 0 (out of memory?)");
    }
    if (frame_a == frame_b || frame_b == frame_c || frame_a == frame_c) {
        panic("pmm self-test: pmm_alloc_frame returned duplicate frames");
    }
    if ((frame_a % PMM_FRAME_SIZE) != 0 || (frame_b % PMM_FRAME_SIZE) != 0 ||
        (frame_c % PMM_FRAME_SIZE) != 0) {
        panic("pmm self-test: allocated frame not page-aligned");
    }

    pmm_free_frame(frame_a);
    pmm_free_frame(frame_b);
    pmm_free_frame(frame_c);

    if (pmm_free_frames() != free_before) {
        panic("pmm self-test: free_frames count did not return to baseline "
              "(before=%lu after=%lu) — leak or double-count",
              (unsigned long)free_before, (unsigned long)pmm_free_frames());
    }

    serial_printf("[boot] pmm self-test passed: allocated/freed 3 distinct frames "
                  "(0x%lx, 0x%lx, 0x%lx), free_frames stable at %lu\n",
                  (unsigned long)frame_a, (unsigned long)frame_b, (unsigned long)frame_c,
                  (unsigned long)pmm_free_frames());

    /*
     * Milestone 4: виртуальная память (4-level VMM).
     * vmm_init() берёт под управление уже работающий PML4 из boot.S.
     */
    vmm_init();

    /* Self-test 1: трансляция адреса внутри статической higher-half
       huge page (сам kernel_main) должна давать физический адрес
       ровно virt - KERNEL_VMA — если это не так, либо boot.S мапит
       не то, что мы думаем, либо vmm_get_phys неверно проходит huge
       page. */
    uint64_t kernel_vma = 0xFFFFFFFF80000000ull;
    uint64_t expected_phys = (uint64_t)(uintptr_t)kernel_main - kernel_vma;
    uint64_t actual_phys = vmm_get_phys((uint64_t)(uintptr_t)kernel_main);
    if (actual_phys != expected_phys) {
        panic("vmm self-test: kernel_main translates to phys 0x%lx, expected 0x%lx",
              (unsigned long)actual_phys, (unsigned long)expected_phys);
    }
    serial_printf("[boot] vmm huge-page translation OK: kernel_main -> phys 0x%lx\n",
                  (unsigned long)actual_phys);

    /* Self-test 2: полный цикл map -> запись -> чтение -> проверка
       трансляции -> unmap -> проверка, что трансляция снова 0.
       Виртуальный адрес заведомо вне статического 1 GiB huge-page
       мапа (0xFFFFFFFF80000000..0xFFFFFFFFBFFFFFFF), поэтому
       vmm_map() реально создаёт новые PDPT/PD/PT записи, а не
       переиспользует существующие. */
    uint64_t test_virt = 0xFFFFFFFFC0000000ull;
    uint64_t test_frame = pmm_alloc_frame();
    if (test_frame == 0) {
        panic("vmm self-test: pmm_alloc_frame failed for test page");
    }

    vmm_map(test_virt, test_frame, VMM_PRESENT | VMM_WRITABLE);

    volatile uint32_t *test_ptr = (volatile uint32_t *)(uintptr_t)test_virt;
    *test_ptr = 0xDEADBEEFu;
    if (*test_ptr != 0xDEADBEEFu) {
        panic("vmm self-test: read-back mismatch after vmm_map (memory not actually usable)");
    }

    uint64_t mapped_phys = vmm_get_phys(test_virt);
    if (mapped_phys != test_frame) {
        panic("vmm self-test: vmm_get_phys returned 0x%lx, expected mapped frame 0x%lx",
              (unsigned long)mapped_phys, (unsigned long)test_frame);
    }

    vmm_unmap(test_virt);
    if (vmm_get_phys(test_virt) != 0) {
        panic("vmm self-test: address still translates after vmm_unmap");
    }

    pmm_free_frame(test_frame); /* self-test не должен утекать память, которую само же и заняло */

    serial_printf("[boot] vmm dynamic map self-test passed: virt 0x%lx <-> phys 0x%lx round-trip OK\n",
                  (unsigned long)test_virt, (unsigned long)test_frame);

    /*
     * Milestone 5: kernel heap (kmalloc/kfree) поверх vmm_map+pmm.
     * Self-test: три выделения разного размера (одно достаточно большое,
     * чтобы гарантированно потребовать расширения кучи новой страницей),
     * запись непересекающихся паттернов, проверка что они не портят друг
     * друга, затем полное освобождение и проверка, что used_bytes
     * вернулся к 0 (иначе — либо утечка, либо двойной счёт).
     */
    kheap_init();

    void *alloc_a = kmalloc(64);
    void *alloc_b = kmalloc(128);
    void *alloc_c = kmalloc(4096); /* заведомо больше остатка первой страницы */

    if (alloc_a == NULL || alloc_b == NULL || alloc_c == NULL) {
        panic("kheap self-test: kmalloc returned NULL unexpectedly");
    }
    if (alloc_a == alloc_b || alloc_b == alloc_c || alloc_a == alloc_c) {
        panic("kheap self-test: kmalloc returned overlapping/duplicate pointers");
    }

    memset(alloc_a, 0xAA, 64);
    memset(alloc_b, 0xBB, 128);
    memset(alloc_c, 0xCC, 4096);

    const uint8_t *pa = (const uint8_t *)alloc_a;
    const uint8_t *pb = (const uint8_t *)alloc_b;
    const uint8_t *pc = (const uint8_t *)alloc_c;
    for (int i = 0; i < 64; i++) {
        if (pa[i] != 0xAA) panic("kheap self-test: alloc_a corrupted at offset %d", i);
    }
    for (int i = 0; i < 128; i++) {
        if (pb[i] != 0xBB) panic("kheap self-test: alloc_b corrupted at offset %d", i);
    }
    for (int i = 0; i < 4096; i++) {
        if (pc[i] != 0xCC) panic("kheap self-test: alloc_c corrupted at offset %d", i);
    }
    serial_write("[boot] kheap: three allocations written/verified without cross-corruption\n");

    kfree(alloc_b); /* сначала середина — проверяет merge с обеих сторон при последующих free */
    kfree(alloc_a);
    kfree(alloc_c);

    if (kheap_used_bytes() != 0) {
        panic("kheap self-test: used_bytes=%lu after freeing everything (leak or double-count)",
              (unsigned long)kheap_used_bytes());
    }
    serial_printf("[boot] kheap self-test passed: used_bytes back to 0, free_bytes=%lu\n",
                  (unsigned long)kheap_free_bytes());

    /*
     * Framebuffer + backbuffer + текстовая консоль поверх них.
     * Раньше framebuffer-тег запрашивался в Multiboot2-заголовке, но
     * никогда не читался и никуда не рисовался — отсюда чёрный экран.
     * fb_init() мапит реальный framebuffer через VMM и выделяет
     * backbuffer; console.c рисует текст 8x16-шрифтом поверх него.
     * Отсутствие графики (нет тега / неподдерживаемый режим) —
     * не фатально, serial-лог по-прежнему работает.
     */
    int have_fb = (fb_init(fb_tag) == 0);
    console_init();
    highlight_init();
    if (have_fb) {
        serial_printf("[boot] framebuffer+console ready: %ux%u\n", fb_width(), fb_height());
    } else {
        serial_write("[boot] framebuffer unavailable — shell will run over serial only\n");
    }

    /*
     * RAM-диск + FAT32.
     * Ramdisk каждую загрузку чистый (обычная память), поэтому том
     * форматируется заново при каждом старте — это ожидаемо для RAM-FS,
     * не баг. fat32_format() сам монтирует то, что записал (round-trip
     * самопроверка), так что отдельный fat32_mount() здесь не нужен.
     */
    struct blockdev *ramdisk = ramdisk_init();
    if (fat32_format(ramdisk) != 0) {
        panic("fat32_format failed on a freshly-initialized ramdisk -- internal bug");
    }

    net_init();
    apxp_init();

    /* Записываем README и (если есть) тестовую ELF64-программу из
       Multiboot2-модуля прямо в FAT32 — это заодно честная проверка
       fat32_write_file() реальными данными, не только нулями. */
    {
        static const char readme_text[] =
            "ApexOS — hobby OS.\n"
            "Это RAM-FAT32 том: пересоздаётся заново при каждой загрузке.\n"
            "Наберите `help` для списка команд. `run HELLO.ELF` запускает\n"
            "тестовую программу в ring3 (см. `help` про её ограничения).\n";
        char readme83[FAT32_NAME_LEN];
        fat32_name_to_83("README.TXT", readme83);
        if (fat32_write_file(fat32_root_cluster(), readme83, readme_text, sizeof(readme_text) - 1) != 0) {
            serial_write("[boot] WARNING: failed writing README.TXT to FAT32\n");
        }

        static const char demo_script[] =
            "# DEMO.ASH -- try: sh DEMO.ASH\n"
            "NAME=world\n"
            "echo Hello $NAME, from an ApexOS script\n"
            "mkdir SCRIPT\n"
            "cd SCRIPT\n"
            "touch NOTE.TXT\n"
            "if [ -f NOTE.TXT ] then\n"
            "    echo NOTE.TXT exists, as expected\n"
            "fi\n"
            "for X in one two three do\n"
            "    echo loop item: $X\n"
            "done\n"
            "ls\n"
            "cd ..\n"
            "echo done\n";
        char demo83[FAT32_NAME_LEN];
        fat32_name_to_83("DEMO.ASH", demo83);
        if (fat32_write_file(fat32_root_cluster(), demo83, demo_script, sizeof(demo_script) - 1) != 0) {
            serial_write("[boot] WARNING: failed writing DEMO.ASH to FAT32\n");
        }
    }

    if (mod_range.count == 0) {
        serial_write("[boot] no multiboot2 modules -- nothing extra available to `run`\n");
    } else {
        /* Второй проход по тем же тегам — теперь уже после того, как
           FAT32 готов. parse_multiboot_tags дешёвая (линейный проход по
           маленькой структуре), повторный вызов безопасен и проще, чем
           хранить все module-теги где-то между двумя фазами загрузки. */
        const struct multiboot_tag_mmap *ignored_mmap = NULL;
        const struct multiboot_tag_framebuffer *ignored_fb = NULL;
        uint32_t ignored_total_size = 0;
        struct module_write_state write_state = { 0 };
        parse_multiboot_tags(mb_info_phys, &ignored_mmap, &ignored_fb,
                              module_write_cb, &write_state, &ignored_total_size);
    }

    serial_write("[boot] handing off to interactive shell (see console/serial for prompt)\n");
    shell_run(); /* не возвращается: либо крутится вечно, либо `run` уходит в ring3 навсегда */
}
