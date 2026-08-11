#include "task.h"
#include "context.h"
#include "process.h"
#include "kheap.h"
#include "panic.h"
#include "serial.h"
#include "string.h"

#define TASK_STACK_SIZE (16u * 1024u)
#define MAX_TASKS 8

enum task_state { TASK_UNUSED, TASK_READY, TASK_DONE };

struct task {
    struct k_jmpbuf ctx;
    uint8_t *stack;
    void (*entry)(void);
    enum task_state state;
    char name[32];
};

static struct task g_tasks[MAX_TASKS];
static int g_ntasks = 0;
static int g_current = -1; /* -1: сейчас исполняется main/shell (не входит в g_tasks[]) */
static int g_preemption_enabled = 0;
static struct k_jmpbuf g_main_ctx; /* контекст main/shell -- участвует в ротации как виртуальный слот g_ntasks */

/* task_trampoline: цель ПЕРВОГО k_longjmp в новую задачу. Это обычная
   C-функция (не ассемблер) — прыжок через k_longjmp просто передаёт
   управление на её адрес с уже настроенным (в task_create) %rsp,
   что полностью эквивалентно тому, как если бы её вызвали обычным
   образом на этом стеке. sti() в начале обязателен: если задача была
   запущена ИЗ обработчика прерывания (таймер), IF там был сброшен. */
static void task_trampoline(void) {
    __asm__ volatile ("sti");
    int id = g_current;
    g_tasks[id].entry();
    g_tasks[id].state = TASK_DONE;
    for (;;) {
        task_yield(); /* завершённая задача просто больше никогда не выбирается планировщиком */
    }
}

int task_create(const char *name, void (*entry)(void)) {
    if (g_ntasks >= MAX_TASKS) {
        serial_write("[task] task_create: no free task slots\n");
        return -1;
    }
    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (stack == NULL) {
        serial_write("[task] task_create: out of memory for stack\n");
        return -1;
    }

    struct task *t = &g_tasks[g_ntasks];
    t->stack = stack;
    t->entry = entry;
    t->state = TASK_READY;
    strcpy_safe(t->name, sizeof(t->name), name);

    uint64_t stack_top = ((uint64_t)(uintptr_t)(stack + TASK_STACK_SIZE)) & ~0xFull;
    t->ctx.rsp = stack_top;
    t->ctx.rip = (uint64_t)(uintptr_t)task_trampoline;
    t->ctx.rbx = t->ctx.rbp = t->ctx.r12 = t->ctx.r13 = t->ctx.r14 = t->ctx.r15 = 0;

    int id = g_ntasks;
    g_ntasks++;
    serial_printf("[task] created task %d (\"%s\")\n", id, name);
    return id;
}

void task_yield(void) {
    if (g_ntasks == 0) {
        return; /* некуда переключаться */
    }

    /* Виртуальный слот g_ntasks представляет main/shell (g_current==-1) —
       ОБЯЗАТЕЛЬНО участвует в ротации как полноценный участник, иначе
       переключение, случившееся изнутри shell (например, из таймера,
       пока shell ждёт ввода), потеряло бы возможность туда вернуться:
       ничто больше не хранило бы точку "где именно был shell". */
    int total = g_ntasks + 1;
    if (total <= 0) {
        panic("task_yield: invalid total=%d (g_ntasks=%d) — refusing to modulo by zero",
              total, g_ntasks);
    }
    int cur_slot = (g_current < 0) ? g_ntasks : g_current;
    struct k_jmpbuf *prev_ctx = (cur_slot == g_ntasks) ? &g_main_ctx : &g_tasks[cur_slot].ctx;

    int rc = k_setjmp(prev_ctx);
    if (rc != 0) {
        return; /* возобновились через k_longjmp -- продолжаем выполнение отсюда */
    }

    for (int i = 1; i <= total; i++) {
        int idx = (cur_slot + i) % total;
        if (idx == g_ntasks) {
            g_current = -1;
            k_longjmp(&g_main_ctx, 1); /* не возвращается */
        }
        if (g_tasks[idx].state == TASK_READY) {
            g_current = idx;
            k_longjmp(&g_tasks[idx].ctx, 1); /* не возвращается */
        }
    }
    /* Не нашли НИКОГО (даже main-слот не подошёл?!) -- этого не может
       произойти, т.к. main-слот всегда "готов" по определению и всегда
       встречается в цикле не позже, чем через total шагов. Оставлено
       как defensive fallthrough, а не unreachable-предположение. */
}

void scheduler_tick(void) {
    if (!g_preemption_enabled) {
        return;
    }
    if (process_is_active()) {
        return; /* ring3-программа сейчас владеет CPU безраздельно -- не мешаем */
    }
    if (g_ntasks == 0) {
        return;
    }
    task_yield();
}

void scheduler_set_preemption(int enabled) {
    g_preemption_enabled = enabled ? 1 : 0;
    serial_printf("[task] preemption %s\n", g_preemption_enabled ? "enabled" : "disabled");
}

int scheduler_preemption_enabled(void) { return g_preemption_enabled; }

int scheduler_task_count(void) { return g_ntasks; }

const char *scheduler_task_name(int index) {
    if (index < 0 || index >= g_ntasks) return "";
    return g_tasks[index].name;
}

int scheduler_task_is_done(int index) {
    if (index < 0 || index >= g_ntasks) return 1;
    return g_tasks[index].state == TASK_DONE;
}
