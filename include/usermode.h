#ifndef APEXOS_USERMODE_H
#define APEXOS_USERMODE_H

#include <stdint.h>

/* enter_usermode: понижает CPU до ring3 и прыгает на entry с указанным
   user_stack_top. Не возвращается в вызвавший код (single-tasking:
   пока нет планировщика, некуда возвращаться). Если iretq всё же
   "вернулся" — это исполнение упадёт в ud2 и вызовет #UD -> panic(). */
__attribute__((noreturn))
void enter_usermode(uint64_t entry, uint64_t user_stack_top);

#endif /* APEXOS_USERMODE_H */
