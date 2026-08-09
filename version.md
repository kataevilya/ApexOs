# Версия ос

###### VERSION -> 1.1
###### BUILD -> 2026-08-09
###### BRANCH -> BETA

---
---
### Типы веток:
###### Release -- стабильная ветка, публичная, для всех. 
###### Current -- нестабильная ветка которая требуется для публичной проверки всех новвоведений перед Release
###### Beta -- еще не готовая версия ОС которая еще дорабатывается и может иметь много проблем

---
### Обновление 1.1:
- Исправлен panic `vmm_map: PT index 128 is already a huge page — cannot subdivide into 4 KiB entries`
- Причина: увеличение статического huge-page мапа boot.S до 1 GiB привело к пересечению динамических VA с 2 MiB huge page записями
- Исправление: все динамические виртуальные адреса (kheap, framebuffer, backbuffer, ramdisk, ELF loader, shell loadbin) сдвинуты за пределы 1 GiB static map
- Затронуты файлы: `kernel/main.c`, `kernel/mm/kheap.c`, `kernel/drivers/fb.c`, `kernel/drivers/ramdisk.c`, `kernel/shell/shell.c`
- Проверка: ISO успешно загружается в QEMU, все self-тесты проходят, shell стартует без паник

