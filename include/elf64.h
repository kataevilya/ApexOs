#ifndef APEXOS_ELF64_H
#define APEXOS_ELF64_H

#include <stdint.h>
#include <stddef.h>

#define ELF_MAG0 0x7F
#define ELF_MAG1 'E'
#define ELF_MAG2 'L'
#define ELF_MAG3 'F'

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EM_X86_64  62
#define ET_EXEC    2

#define PT_LOAD 1

struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

/*
 * elf64_load: парсит ELF64-образ в памяти (data, size байт — например,
 * physical-адрес Multiboot2-модуля) и мапит все PT_LOAD сегменты как
 * пользовательские страницы через vmm_map()+pmm_alloc_frame().
 *
 * Честное ограничение этого этапа: ВСЕ сегменты мапятся
 * VMM_PRESENT|VMM_WRITABLE|VMM_USER независимо от p_flags — то есть
 * .text реально доступен на запись из userspace. NX/read-only ещё не
 * применяются (EFER.NXE не включён, и мы не читаем p_flags вообще).
 * Это осознанно отложено, а не тихо забыто — исправляется вместе с
 * будущим NX-bit/W^X milestone.
 *
 * Возвращает 0 при успехе (заполняя *out_entry и *out_user_stack_top),
 * -1 при любой структурной проблеме файла — в этом случае вызывающий
 * код решает сам (panic либо отказ от запуска), elf64_load сам не
 * паникует на данных ИЗ ФАЙЛА (в отличие от данных из самого ядра),
 * т.к. битый/чужой ELF — это ожидаемая, а не программная ошибка.
 */
int elf64_load(const void *data, size_t size, uint64_t *out_entry, uint64_t *out_user_stack_top);

/*
 * elf64_unload: освобождает ВСЕ страницы (сегменты + стек), замапленные
 * последним успешным elf64_load() — unmap + pmm_free_frame для каждой.
 * Без этого повторный `run` того же (или любого другого) файла падал
 * бы в panic() при попытке vmm_map поверх уже занятого виртуального
 * адреса (0x400000 и т.п. никогда не освобождались раньше — это и
 * была причина "работает один раз, потом ломается"). Вызывается
 * process_run() автоматически после завершения программы — обычному
 * коду дёргать эту функцию самому не нужно.
 */
void elf64_unload(void);

#endif /* APEXOS_ELF64_H */
