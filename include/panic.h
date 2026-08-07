#ifndef APEXOS_PANIC_H
#define APEXOS_PANIC_H

/* panic() логирует причину через serial и останавливает CPU навсегда
   (halt_forever, определён в entry.S). Не возвращается — вызывающему
   коду не нужно писать код "после паники". */
__attribute__((noreturn))
void panic(const char *fmt, ...);

#endif /* APEXOS_PANIC_H */
