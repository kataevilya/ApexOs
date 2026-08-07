#include "elf64.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"

/* Фиксированный адрес вершины пользовательского стека и его размер —
   годится для крошечных тестовых программ на этом этапе; per-process
   настройка (ASLR, размер по ELF-заголовку и т.п.) — не здесь. */
#define USER_STACK_TOP   0x0000000000800000ull
#define USER_STACK_PAGES 4u

/* Отслеживаем каждую виртуальную страницу, замапленную ПОСЛЕДНИМ
   elf64_load() — без этого повторный `run` того же (или любого
   другого) файла падал бы в panic() в vmm_map при попытке замапить
   уже занятый адрес (0x400000 и т.п. никогда не освобождались).
   Список сбрасывается в начале каждого elf64_load(), поэтому
   elf64_unload() ОБЯЗАН вызываться (см. process.c) сразу после
   завершения программы, до следующего elf64_load. */
#define MAX_TRACKED_PAGES 4096 /* 16 MiB программы+стека с запасом для тестовых программ */
static uint64_t g_tracked_pages[MAX_TRACKED_PAGES];
static int g_tracked_count = 0;

static int track_page(uint64_t virt) {
    if (g_tracked_count >= MAX_TRACKED_PAGES) {
        serial_write("[elf64] WARNING: page tracking limit reached — "
                      "this page won't be freed on unload (leak, not a crash)\n");
        return -1;
    }
    g_tracked_pages[g_tracked_count++] = virt;
    return 0;
}

void elf64_unload(void) {
    for (int i = 0; i < g_tracked_count; i++) {
        uint64_t virt = g_tracked_pages[i];
        uint64_t phys = vmm_get_phys(virt);
        if (phys != 0) {
            vmm_unmap(virt);
            pmm_free_frame(phys);
        }
    }
    if (g_tracked_count > 0) {
        serial_printf("[elf64] unloaded: freed %d page(s)\n", g_tracked_count);
    }
    g_tracked_count = 0;
}

static int validate_header(const struct elf64_ehdr *eh, size_t size) {
    if (size < sizeof(struct elf64_ehdr)) {
        serial_write("[elf64] file too small for ELF header\n");
        return -1;
    }
    if (eh->e_ident[0] != ELF_MAG0 || eh->e_ident[1] != ELF_MAG1 ||
        eh->e_ident[2] != ELF_MAG2 || eh->e_ident[3] != ELF_MAG3) {
        serial_write("[elf64] bad magic\n");
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64) {
        serial_write("[elf64] not an ELF64 file\n");
        return -1;
    }
    if (eh->e_ident[5] != ELFDATA2LSB) {
        serial_write("[elf64] not little-endian\n");
        return -1;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_printf("[elf64] unexpected e_machine=%u (expected x86_64)\n", (unsigned)eh->e_machine);
        return -1;
    }
    if (eh->e_type != ET_EXEC) {
        serial_printf("[elf64] unexpected e_type=%u (only ET_EXEC supported -- no PIE/shared yet)\n",
                      (unsigned)eh->e_type);
        return -1;
    }
    if (eh->e_phentsize != sizeof(struct elf64_phdr)) {
        serial_printf("[elf64] unexpected e_phentsize=%u\n", (unsigned)eh->e_phentsize);
        return -1;
    }
    if (eh->e_phnum == 0) {
        serial_write("[elf64] no program headers\n");
        return -1;
    }

    uint64_t phdr_table_bytes = (uint64_t)eh->e_phnum * eh->e_phentsize;
    if (phdr_table_bytes / eh->e_phentsize != eh->e_phnum) {
        serial_write("[elf64] e_phnum * e_phentsize overflows\n");
        return -1;
    }
    if (eh->e_phoff > size || phdr_table_bytes > size - eh->e_phoff) {
        serial_write("[elf64] program header table lies outside the file\n");
        return -1;
    }
    return 0;
}

int elf64_load(const void *data, size_t size, uint64_t *out_entry, uint64_t *out_user_stack_top) {
    const uint8_t *base = (const uint8_t *)data;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)data;

    /* Свежий список отслеживаемых страниц для ЭТОЙ загрузки. Если
       предыдущая программа не была выгружена через elf64_unload() —
       её страницы тут молча "теряются" из списка (продолжат числиться
       занятыми в VMM/PMM, просто больше не сможем их свободить через
       эту функцию). process_run() в process.c гарантирует правильный
       порядок вызовов, так что на практике это не происходит. */
    g_tracked_count = 0;

    if (validate_header(eh, size) != 0) {
        return -1;
    }

    const struct elf64_phdr *phdrs = (const struct elf64_phdr *)(base + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        if (ph->p_filesz > ph->p_memsz) {
            serial_printf("[elf64] segment %u: p_filesz > p_memsz\n", (unsigned)i);
            return -1;
        }
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset) {
            serial_printf("[elf64] segment %u: file range lies outside the file\n", (unsigned)i);
            return -1;
        }

        uint64_t seg_start = ph->p_vaddr;
        uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
        if (seg_end < seg_start) {
            serial_printf("[elf64] segment %u: p_vaddr + p_memsz overflows\n", (unsigned)i);
            return -1;
        }

        uint64_t page_start = seg_start & ~(uint64_t)(VMM_PAGE_SIZE - 1);
        uint64_t page_end = (seg_end + VMM_PAGE_SIZE - 1) & ~(uint64_t)(VMM_PAGE_SIZE - 1);

        for (uint64_t v = page_start; v < page_end; v += VMM_PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (frame == 0) {
                serial_write("[elf64] out of physical memory while loading a segment\n");
                return -1;
            }
            /* Честное ограничение: мапим VMM_USER|VMM_WRITABLE для ВСЕХ
               сегментов, не читая p_flags — .text реально писабелен из
               userspace на этом этапе. NX/read-only enforcement —
               отдельный будущий milestone (нужен EFER.NXE + разбор
               p_flags), не делаю вид что оно уже применяется. */
            vmm_map(v, frame, VMM_PRESENT | VMM_WRITABLE | VMM_USER);
            track_page(v);

            /* Обнуляем всю страницу перед копированием — иначе часть
               физического фрейма (за пределами filesz, т.е. .bss) могла
               бы содержать старые данные ядра/предыдущего использования
               этого фрейма, случайно "утёкшие" в userspace. */
            uint8_t *page_ptr = (uint8_t *)(uintptr_t)v;
            for (uint64_t z = 0; z < VMM_PAGE_SIZE; z++) {
                page_ptr[z] = 0;
            }
        }

        const uint8_t *src = base + ph->p_offset;
        uint8_t *dst = (uint8_t *)(uintptr_t)seg_start;
        for (uint64_t b = 0; b < ph->p_filesz; b++) {
            dst[b] = src[b];
        }

        serial_printf("[elf64] segment %u loaded: vaddr=0x%lx filesz=%lu memsz=%lu\n",
                      (unsigned)i, (unsigned long)seg_start,
                      (unsigned long)ph->p_filesz, (unsigned long)ph->p_memsz);
    }

    uint64_t stack_bottom = USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * VMM_PAGE_SIZE;
    for (uint64_t v = stack_bottom; v < USER_STACK_TOP; v += VMM_PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            serial_write("[elf64] out of physical memory allocating user stack\n");
            return -1;
        }
        vmm_map(v, frame, VMM_PRESENT | VMM_WRITABLE | VMM_USER);
        track_page(v);
    }

    *out_entry = eh->e_entry;
    *out_user_stack_top = USER_STACK_TOP;
    serial_printf("[elf64] loaded OK: entry=0x%lx user_stack_top=0x%lx (%u pages)\n",
                  (unsigned long)eh->e_entry, (unsigned long)USER_STACK_TOP,
                  (unsigned)USER_STACK_PAGES);
    return 0;
}
