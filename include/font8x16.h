#ifndef APEXOS_FONT8X16_H
#define APEXOS_FONT8X16_H

#include <stdint.h>

#define FONT8X16_WIDTH  8
#define FONT8X16_HEIGHT 16
#define FONT8X16_FIRST_CHAR 32   /* space */
#define FONT8X16_LAST_CHAR  126  /* '~' */
#define FONT8X16_NUM_GLYPHS (FONT8X16_LAST_CHAR - FONT8X16_FIRST_CHAR + 1)

/* Растеризовано программно из DejaVu Sans Mono (см. tools/gen_font.py),
   не набрано вручную по памяти — так исключён риск ошибок транскрипции
   битмапов. Один байт на строку глифа, старший бит — самый левый пиксель.
   Символы вне [32,126] не покрыты — рендерер обязан сам решать, чем их
   заменять (см. console.c). */
extern const uint8_t font8x16_data[FONT8X16_NUM_GLYPHS][FONT8X16_HEIGHT];

#endif /* APEXOS_FONT8X16_H */
