#ifndef APEXOS_KEYBOARD_H
#define APEXOS_KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

/* "Псевдо-символы" для клавиш/сочетаний, у которых нет ASCII-кода —
   значения нарочно выше 0xFF, чтобы не пересекаться с обычными байтами
   ASCII/расширенной латиницы, приходящими через keyboard_read_key(). */
#define KEY_NONE    (-1)
#define KEY_UP      0x100
#define KEY_DOWN    0x101
#define KEY_LEFT    0x102
#define KEY_RIGHT   0x103
#define KEY_HOME    0x104
#define KEY_END     0x105
#define KEY_DELETE  0x106
#define KEY_CTRL_S  0x107
#define KEY_CTRL_X  0x108
#define KEY_CTRL_R  0x109
#define KEY_F2      0x10A
#define KEY_CTRL_SHIFT_ESC 0x10B

/* keyboard_init: регистрирует обработчик IRQ1 (вектор 33). Не
   размаскирует IRQ сама — вызывающий код делает это явно после того,
   как убедится, что IDT/PIC готовы (тот же паттерн, что и с PIT). */
void keyboard_init(void);

/* keyboard_read_key: возвращает KEY_NONE, если очередь пуста, иначе
   либо код ASCII-символа (0..255), либо один из KEY_* выше. НЕ
   блокирует. */
int keyboard_read_key(void);

/* Старый интерфейс "только обычные символы" — оставлен ради shell'а,
   которому не нужны стрелки/Ctrl. Молча пропускает (теряет) события
   KEY_*: для обычного ввода команд это не проблема — просто нажатие
   стрелки в приглашении shell ничего не делает, а не падает. */
int keyboard_read_char(char *out);

#endif /* APEXOS_KEYBOARD_H */
