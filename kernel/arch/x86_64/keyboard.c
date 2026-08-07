#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "serial.h"

#define PS2_DATA_PORT 0x60

/* Кольцевой буфер СОБЫТИЙ (не байтов) — пишет IRQ1, читает основной
   код. int вместо char: значения KEY_* (стрелки, Ctrl+буква) не
   помещаются в один байт ASCII без пересечения с реальными кодами. */
#define KB_BUFFER_SIZE 128
static volatile int g_kb_buffer[KB_BUFFER_SIZE];
static volatile uint32_t g_kb_head = 0;
static volatile uint32_t g_kb_tail = 0;

static int g_shift_pressed = 0;
static int g_ctrl_pressed = 0;
static int g_extended_prefix = 0; /* увидели 0xE0, следующий байт — расширенный код */

/*
 * US QWERTY, scancode set 1, make-коды 0x00-0x39 — этого достаточно для
 * базового ввода в shell/редактор. НЕ реализовано (честно, не тихо
 * пропущено): Caps Lock, keypad/NumLock, другие раскладки.
 */
static const char scancode_to_ascii[] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8',
    '9',  '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',
    't',  'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0,   '\\','z', 'x', 'c', 'v', 'b', 'n',
    'm',  ',', '.', '/', 0,   '*', 0,   ' ',
};

static const char scancode_to_ascii_shift[] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*',
    '(',  ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',
    'T',  'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A',  'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M',  '<', '>', '?', 0,   '*', 0,   ' ',
};

#define SCANCODE_LSHIFT 0x2A
#define SCANCODE_RSHIFT 0x36
#define SCANCODE_CTRL   0x1D /* LCtrl без extended-префикса; RCtrl приходит как 0xE0,0x1D —
                                 обрабатываем оба одинаково, отдельная RCtrl-семантика не нужна */
#define SCANCODE_RELEASE_BIT 0x80

static void kb_push_event(int event) {
    uint32_t next = (g_kb_head + 1) % KB_BUFFER_SIZE;
    if (next == g_kb_tail) {
        g_kb_tail = (g_kb_tail + 1) % KB_BUFFER_SIZE; /* буфер полон — теряем самое старое, не блокируем IRQ */
    }
    g_kb_buffer[g_kb_head] = event;
    g_kb_head = next;
}

static void keyboard_irq_handler(struct registers *regs) {
    (void)regs;
    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode == 0xE0) {
        g_extended_prefix = 1;
        return;
    }
    int extended = g_extended_prefix;
    g_extended_prefix = 0;

    int is_release = (scancode & SCANCODE_RELEASE_BIT) != 0;
    uint8_t code = scancode & (uint8_t)~SCANCODE_RELEASE_BIT;

    if (!extended && (code == SCANCODE_LSHIFT || code == SCANCODE_RSHIFT)) {
        g_shift_pressed = !is_release;
        return;
    }
    if (code == SCANCODE_CTRL) {
        g_ctrl_pressed = !is_release;
        return;
    }
    if (!extended && code == 0x3C) { /* F2 make code, scancode set 1 */
        if (!is_release) {
            kb_push_event(KEY_F2);
        }
        return;
    }
    if (is_release) {
        return;
    }

    if (extended) {
        int key = KEY_NONE;
        switch (code) {
            case 0x48: key = KEY_UP; break;
            case 0x50: key = KEY_DOWN; break;
            case 0x4B: key = KEY_LEFT; break;
            case 0x4D: key = KEY_RIGHT; break;
            case 0x47: key = KEY_HOME; break;
            case 0x4F: key = KEY_END; break;
            case 0x53: key = KEY_DELETE; break;
            default: break; /* прочие расширенные коды (PrtScn, медиа-клавиши и т.п.) пока не нужны */
        }
        if (key != KEY_NONE) {
            kb_push_event(key);
        }
        return;
    }

    if (g_ctrl_pressed) {
        if (code == 0x01 && g_shift_pressed) { /* Esc scancode, Shift also held */
            kb_push_event(KEY_CTRL_SHIFT_ESC);
            return;
        }
        if (code >= sizeof(scancode_to_ascii)) {
            return;
        }
        char c = scancode_to_ascii[code]; /* Ctrl-комбинации не зависят от Shift */
        if (c == 's') { kb_push_event(KEY_CTRL_S); return; }
        if (c == 'x') { kb_push_event(KEY_CTRL_X); return; }
        if (c == 'r') { kb_push_event(KEY_CTRL_R); return; }
        return; /* прочие Ctrl+буква пока не назначены — молча игнорируем, не как обычный символ */
    }

    if (code >= sizeof(scancode_to_ascii)) {
        serial_printf("[kbd] unmapped scancode 0x%x\n", (unsigned)code);
        return;
    }

    char c = g_shift_pressed ? scancode_to_ascii_shift[code] : scancode_to_ascii[code];
    if (c != 0) {
        kb_push_event((int)(unsigned char)c);
    }
}

void keyboard_init(void) {
    g_kb_head = 0;
    g_kb_tail = 0;
    g_shift_pressed = 0;
    g_ctrl_pressed = 0;
    g_extended_prefix = 0;
    register_interrupt_handler(33, keyboard_irq_handler); /* IRQ1 -> vector 33 */
    serial_write("[kbd] IRQ1 handler registered (US QWERTY, scancode set 1, arrows+Ctrl)\n");
}

int keyboard_read_key(void) {
    if (g_kb_tail == g_kb_head) {
        return KEY_NONE;
    }
    int ev = g_kb_buffer[g_kb_tail];
    g_kb_tail = (g_kb_tail + 1) % KB_BUFFER_SIZE;
    return ev;
}

int keyboard_read_char(char *out) {
    int ev = keyboard_read_key();
    if (ev == KEY_NONE) {
        return 0;
    }
    if (ev >= 0 && ev < 256) {
        *out = (char)ev;
        return 1;
    }
    return 0; /* специальная клавиша (стрелка/Ctrl+буква) — обычному line-based вводу не нужна, теряем */
}
