#include "kheap.h"
#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "serial.h"

/* Виртуальный диапазон кучи — заведомо не пересекается ни с образом
   ядра (KERNEL_VMA..+1 GiB), ни с адресами, которые уже трогали
   self-тесты VMM (0xFFFFFFFFC0000000, освобождён обратно unmap'ом,
   но выбираем другой диапазон всё равно, чтобы не полагаться на это). */
#define HEAP_VIRT_START 0xFFFFFFFFD0000000ull
#define HEAP_MAX_BYTES  (16ull * 1024 * 1024) /* 16 MiB — явный предел, не "растёт вечно" */

#define BLOCK_MAGIC 0xB10CB10Cu
#define MIN_SPLIT_REMAINDER 32 /* ниже этого — отдаём блок целиком, не дробим */

struct block_header {
    uint32_t magic;
    uint32_t free;
    size_t size; /* полезный размер, БЕЗ учёта самого заголовка */
    struct block_header *prev;
    struct block_header *next;
};

static uint64_t g_heap_mapped_end = HEAP_VIRT_START; /* граница уже замапленной памяти */
static struct block_header *g_head = NULL;
static struct block_header *g_tail = NULL;
static uint64_t g_used_bytes = 0;
static uint64_t g_free_bytes = 0;

static inline size_t align_up(size_t value, size_t align) {
    return (value + (align - 1)) & ~(align - 1);
}

void kheap_init(void) {
    g_heap_mapped_end = HEAP_VIRT_START;
    g_head = NULL;
    g_tail = NULL;
    g_used_bytes = 0;
    g_free_bytes = 0;
    serial_printf("[kheap] initialized, virt range [0x%lx, 0x%lx + %lu MiB max)\n",
                  (unsigned long)HEAP_VIRT_START, (unsigned long)HEAP_VIRT_START,
                  (unsigned long)(HEAP_MAX_BYTES / (1024 * 1024)));
}

/* expand_heap: домаппливает достаточно новых страниц, чтобы получить
   один свободный блок размером минимум min_user_bytes, и добавляет
   его в конец списка блоков. */
static struct block_header *expand_heap(size_t min_user_bytes) {
    size_t needed = sizeof(struct block_header) + min_user_bytes;
    size_t pages_needed = (needed + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    size_t grow_bytes = pages_needed * VMM_PAGE_SIZE;

    uint64_t new_end = g_heap_mapped_end + grow_bytes;
    if (new_end < g_heap_mapped_end) { /* переполнение uint64_t — практически недостижимо, но проверяем */
        panic("kheap: heap growth overflowed address arithmetic");
    }
    if (new_end - HEAP_VIRT_START > HEAP_MAX_BYTES) {
        panic("kheap: would exceed %lu MiB heap limit (requested %lu bytes)",
              (unsigned long)(HEAP_MAX_BYTES / (1024 * 1024)), (unsigned long)min_user_bytes);
    }

    uint64_t block_virt = g_heap_mapped_end;
    for (uint64_t v = g_heap_mapped_end; v < new_end; v += VMM_PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            panic("kheap: out of physical memory while growing heap "
                  "(requested %lu bytes)", (unsigned long)min_user_bytes);
        }
        vmm_map(v, frame, VMM_PRESENT | VMM_WRITABLE);
    }
    g_heap_mapped_end = new_end;

    struct block_header *block = (struct block_header *)(uintptr_t)block_virt;
    block->magic = BLOCK_MAGIC;
    block->free = 1;
    block->size = grow_bytes - sizeof(struct block_header);
    block->prev = g_tail;
    block->next = NULL;

    if (g_tail != NULL) {
        g_tail->next = block;
    } else {
        g_head = block;
    }
    g_tail = block;
    g_free_bytes += block->size;

    return block;
}

static struct block_header *find_free_block(size_t size) {
    for (struct block_header *b = g_head; b != NULL; b = b->next) {
        if (b->free && b->size >= size) {
            return b;
        }
    }
    return NULL;
}

/* Если после выделения size байт из block остаётся достаточно места
   на отдельный полезный блок — отрезаем "хвост" в новый свободный блок,
   вставленный сразу за block в списке. Иначе — оставляем внутреннюю
   фрагментацию (дробить смысла нет, заголовок нового блока сам по
   себе стоил бы дороже выгоды). */
static void maybe_split(struct block_header *block, size_t size) {
    size_t remainder = block->size - size;
    if (remainder < sizeof(struct block_header) + MIN_SPLIT_REMAINDER) {
        return;
    }

    uint8_t *block_data = (uint8_t *)(block + 1);
    struct block_header *new_block = (struct block_header *)(block_data + size);
    new_block->magic = BLOCK_MAGIC;
    new_block->free = 1;
    new_block->size = remainder - sizeof(struct block_header);
    new_block->prev = block;
    new_block->next = block->next;

    if (block->next != NULL) {
        block->next->prev = new_block;
    } else {
        g_tail = new_block;
    }
    block->next = new_block;
    block->size = size;

    g_free_bytes += new_block->size;
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size_t aligned = align_up(size, 16);

    struct block_header *block = find_free_block(aligned);
    if (block == NULL) {
        block = expand_heap(aligned); /* уже добавил block->size в g_free_bytes */
    }
    /* В обоих случаях (найден существующий свободный блок, или только что
       создан expand_heap) block->size сейчас — это ПОЛНЫЙ размер свободного
       блока, который мы собираемся потребить/раздробить. Убираем его из
       free_bytes целиком; maybe_split() вернёт назад ту часть, что окажется
       отрезанным свободным остатком. Раньше это вычитание было только в
       ветке "существующий блок", отчего для свежесозданных expand_heap
       блоков получался двойной счёт (одновременно free и used). */
    g_free_bytes -= block->size;

    maybe_split(block, aligned);

    block->free = 0;
    g_used_bytes += block->size;

    return (void *)(block + 1);
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return; /* как и стандартный free(NULL) — штатный no-op */
    }

    struct block_header *block = (struct block_header *)ptr - 1;
    if (block->magic != BLOCK_MAGIC) {
        panic("kfree: corrupted or invalid block header at %p (magic=0x%x, expected 0x%x) — "
              "buffer overflow before this allocation, or ptr not from kmalloc",
              ptr, (unsigned)block->magic, (unsigned)BLOCK_MAGIC);
    }
    if (block->free) {
        panic("kfree: double free of block at %p", ptr);
    }

    block->free = 1;
    g_used_bytes -= block->size;
    g_free_bytes += block->size;

    /* Слияние с следующим блоком (по адресу), если он тоже свободен. */
    if (block->next != NULL && block->next->free) {
        struct block_header *next = block->next;
        block->size += sizeof(struct block_header) + next->size;
        block->next = next->next;
        if (next->next != NULL) {
            next->next->prev = block;
        } else {
            g_tail = block;
        }
        g_free_bytes += sizeof(struct block_header); /* заголовок next перестал существовать как отдельный блок */
    }

    /* Слияние с предыдущим блоком (по адресу), если он тоже свободен. */
    if (block->prev != NULL && block->prev->free) {
        struct block_header *prev = block->prev;
        prev->size += sizeof(struct block_header) + block->size;
        prev->next = block->next;
        if (block->next != NULL) {
            block->next->prev = prev;
        } else {
            g_tail = prev;
        }
        g_free_bytes += sizeof(struct block_header);
    }
}

uint64_t kheap_used_bytes(void) { return g_used_bytes; }
uint64_t kheap_free_bytes(void) { return g_free_bytes; }
