# Версия ос

###### VERSION -> 1.2
###### BUILD -> 2026-08-10
###### BRANCH -> BETA

---
---
### Типы веток:
###### Release -- стабильная ветка, публичная, для всех. 
###### Current -- нестабильная ветка которая требуется для публичной проверки всех новвоведений перед Release
###### Beta -- еще не готовая версия ОС которая еще дорабатывается и может иметь много проблем

---
### Обновление 1.2:
- Добавлен сетевой стек: драйвер RTL8139, Ethernet, ARP, IP, ICMP, UDP, TCP
- Добавлен DHCP-клиент с fallback на 10.0.2.15/24
- Добавлен DNS-резолвер и HTTP-клиент
- Добавлен пакетный менеджер APXP с командами `dlw`, `apxprun`, `pkglist`
- Исправлен парсинг URL в `pkgmgr_download` для поддержки коротких ссылок вида `https://github.com/user/repo/file.apxp`
- Исправлена блокировка клавиатуры после загрузки: клавиатура теперь работает в shell
- Затронуты файлы: `include/net.h`, `include/rtl8139.h`, `include/apxp.h`, `kernel/drivers/net/rtl8139.c`, `kernel/net/*.c`, `kernel/apxp/*.c`, `kernel/shell/shell.c`, `kernel/main.c`
- Проверка: ISO загружается в QEMU, сеть инициализируется, `dlw` скачивает APXP-пакеты

---
### Обновление 1.1:
- Исправлен panic `vmm_map: PT index 128 is already a huge page — cannot subdivide into 4 KiB entries`
- Причина: увеличение статического huge-page мапа boot.S до 1 GiB привело к пересечению динамических VA с 2 MiB huge page записями
- Исправление: все динамические виртуальные адреса (kheap, framebuffer, backbuffer, ramdisk, ELF loader, shell loadbin) сдвинуты за пределы 1 GiB static map
- Затронуты файлы: `kernel/main.c`, `kernel/mm/kheap.c`, `kernel/drivers/fb.c`, `kernel/drivers/ramdisk.c`, `kernel/shell/shell.c`
- Проверка: ISO успешно загружается в QEMU, все self-тесты проходят, shell стартует без паник
