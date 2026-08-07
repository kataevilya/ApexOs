#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "serial.h"
#include "mm_layout.h"
#include <stddef.h>

/* boot_pml4 — уже работающий PML4, построенный в boot.S. Символ линкован
   по низкому (физическому) адресу, т.к. .boot.data не попадает под
   higher-half смещение linker.ld — поэтому его адрес, видимый из C,
   уже равен физическому адресу, и в него можно писать напрямую
   (identity-mapping этот диапазон покрывает). */
extern uint8_t boot_pml4[];

#define PDE_PS_BIT (1ull << 7) /* huge page (2 MiB на уровне PD) */
#define ENTRY_ADDR_MASK (~0xFFFull)

static uint64_t g_pml4_phys = 0;

/*
 * phys_to_ptr: единственная точка, где физический адрес превращается
 * в указатель, который реально можно разыменовать. Гарантирует, что
 * мы никогда не попытаемся прочитать/записать физическую страницу,
 * для которой нет ни одного действующего отображения — вместо этого
 * честный panic() (см. обоснование в vmm.h).
 */
static inline uint64_t *phys_to_ptr(uint64_t phys) {
    if (phys >= APEXOS_STATIC_MAP_BYTES) {
        panic("vmm: physical address 0x%lx is outside the statically-mapped "
              "%lu MiB window (no physical direct-map yet)",
              (unsigned long)phys, (unsigned long)(APEXOS_STATIC_MAP_BYTES / (1024 * 1024)));
    }
    return (uint64_t *)(uintptr_t)phys;
}

static inline void invalidate_page(uint64_t virt) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_init(void) {
    g_pml4_phys = (uint64_t)(uintptr_t)boot_pml4;
    serial_printf("[vmm] managing existing PML4 at phys 0x%lx (built in boot.S)\n",
                  (unsigned long)g_pml4_phys);
}

/*
 * step_create: возвращает указатель на таблицу следующего уровня по
 * заданному индексу в table, создавая её через pmm_alloc_frame(), если
 * она ещё не существует. panic(), если индекс уже занят huge page —
 * подмена гранулярности "на лету" не поддерживается и не должна
 * происходить молча.
 */
static uint64_t *step_create(uint64_t *table, uint64_t index, const char *level_name) {
    uint64_t entry = table[index];

    if (entry & VMM_PRESENT) {
        if (entry & PDE_PS_BIT) {
            panic("vmm_map: %s index %lu is already a huge page — "
                  "cannot subdivide into 4 KiB entries",
                  level_name, (unsigned long)index);
        }
        return phys_to_ptr(entry & ENTRY_ADDR_MASK);
    }

    uint64_t frame = pmm_alloc_frame();
    if (frame == 0) {
        panic("vmm_map: out of physical memory allocating a %s table", level_name);
    }

    uint64_t *new_table = phys_to_ptr(frame);
    for (int i = 0; i < 512; i++) {
        new_table[i] = 0;
    }

    /* Промежуточные уровни намеренно максимально разрешающие —
       реальные права применяются только на последнем (PT) уровне. */
    table[index] = frame | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    return new_table;
}

/* step_existing: как step_create, но НИЧЕГО не создаёт — возвращает
   NULL, если записи нет. Используется unmap/get_phys, которым не
   нужно (и не следует) достраивать таблицы ради запроса/удаления. */
static uint64_t *step_existing(uint64_t *table, uint64_t index, uint64_t *out_entry) {
    uint64_t entry = table[index];
    *out_entry = entry;
    if (!(entry & VMM_PRESENT)) {
        return NULL;
    }
    if (entry & PDE_PS_BIT) {
        return NULL; /* huge page — вызывающий код обрабатывает сам через out_entry */
    }
    return phys_to_ptr(entry & ENTRY_ADDR_MASK);
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virt % VMM_PAGE_SIZE != 0) {
        panic("vmm_map: virt 0x%lx is not page-aligned", (unsigned long)virt);
    }
    if (phys % VMM_PAGE_SIZE != 0) {
        panic("vmm_map: phys 0x%lx is not page-aligned", (unsigned long)phys);
    }
    if (phys == 0) {
        panic("vmm_map: refusing to map physical frame 0 (permanently reserved)");
    }

    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = phys_to_ptr(g_pml4_phys);
    uint64_t *pdpt = step_create(pml4, pml4_i, "PDPT");
    uint64_t *pd   = step_create(pdpt, pdpt_i, "PD");
    uint64_t *pt   = step_create(pd, pd_i, "PT");

    if (pt[pt_i] & VMM_PRESENT) {
        panic("vmm_map: virt 0x%lx is already mapped — unmap before remapping",
              (unsigned long)virt);
    }

    pt[pt_i] = (phys & ENTRY_ADDR_MASK) | (flags & 0xFFFull) | VMM_PRESENT;
    invalidate_page(virt);
}

void vmm_unmap(uint64_t virt) {
    if (virt % VMM_PAGE_SIZE != 0) {
        panic("vmm_unmap: virt 0x%lx is not page-aligned", (unsigned long)virt);
    }

    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t entry;
    uint64_t *pml4 = phys_to_ptr(g_pml4_phys);

    uint64_t *pdpt = step_existing(pml4, pml4_i, &entry);
    if (pdpt == NULL) {
        panic("vmm_unmap: virt 0x%lx was never mapped (no PDPT)", (unsigned long)virt);
    }
    uint64_t *pd = step_existing(pdpt, pdpt_i, &entry);
    if (pd == NULL) {
        panic("vmm_unmap: virt 0x%lx was never mapped (no PD)", (unsigned long)virt);
    }
    uint64_t *pt = step_existing(pd, pd_i, &entry);
    if (pt == NULL) {
        if (entry & PDE_PS_BIT) {
            panic("vmm_unmap: virt 0x%lx falls inside a static huge page — "
                  "not individually unmappable", (unsigned long)virt);
        }
        panic("vmm_unmap: virt 0x%lx was never mapped (no PT)", (unsigned long)virt);
    }

    if (!(pt[pt_i] & VMM_PRESENT)) {
        panic("vmm_unmap: virt 0x%lx was never mapped (PTE not present)", (unsigned long)virt);
    }

    pt[pt_i] = 0;
    invalidate_page(virt);
}

uint64_t vmm_get_phys(uint64_t virt) {
    uint64_t offset_in_page = virt & 0xFFFull;
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t entry;
    uint64_t *pml4 = phys_to_ptr(g_pml4_phys);

    uint64_t *pdpt = step_existing(pml4, pml4_i, &entry);
    if (pdpt == NULL) {
        return 0; /* не замаплено — нормальный результат запроса, не ошибка */
    }
    uint64_t *pd = step_existing(pdpt, pdpt_i, &entry);
    if (pd == NULL) {
        return 0;
    }
    uint64_t *pt = step_existing(pd, pd_i, &entry);
    if (pt == NULL) {
        if (entry & PDE_PS_BIT) {
            /* Статическая 2 MiB huge page из boot.S — считаем адрес
               честно, а не отказываемся отвечать только потому, что
               map/unmap с huge pages не работают на этом уровне. */
            uint64_t huge_base = entry & ~0x1FFFFFull;
            return huge_base + (virt & 0x1FFFFFull);
        }
        return 0;
    }

    if (!(pt[pt_i] & VMM_PRESENT)) {
        return 0;
    }
    return (pt[pt_i] & ENTRY_ADDR_MASK) + offset_in_page;
}
