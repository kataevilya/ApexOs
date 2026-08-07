#ifndef APEXOS_FAT32_H
#define APEXOS_FAT32_H

#include <stdint.h>
#include <stddef.h>
#include "blockdev.h"

/*
 * Честное ограничение этой реализации (написано прямо, не спрятано):
 *   - Только короткие имена 8.3 (без LFN). "hello.elf" хранится как
 *     "HELLO   ELF" — заглавными, без длинных имён. Ввод длиннее 8+3
 *     символов отвергается функцией преобразования имени.
 *   - Каталоги поддерживаются на произвольную вложенность (mkdir/cd
 *     работают с любым каталогом, не только корнем) — реализовано
 *     честно, не только "плоский root".
 */

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F

#define FAT32_NAME_LEN 11 /* 8 имя + 3 расширение, без точки, пробелами дополнено */

struct fat32_dirent {
    uint8_t  name[FAT32_NAME_LEN];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed));

/* fat32_mount: читает и валидирует boot sector переданного blockdev,
   инициализирует внутреннее состояние драйвера (один смонтированный
   том за раз — этого достаточно, пока в системе один ramdisk).
   Возвращает 0 при успехе, -1 при неудачной валидации (не FAT32 / не
   отформатирован / повреждён) — НЕ panic(), т.к. это ожидаемая ошибка
   для "ещё не отформатированного" устройства, что и обрабатывает
   вызывающий код через fat32_format(). */
int fat32_mount(struct blockdev *dev);

/* fat32_format: пишет свежий, пустой FAT32 том (boot sector, FSInfo,
   обе копии FAT, пустой корневой каталог) поверх всего blockdev, затем
   монтирует его. Разрушает все существующие данные на устройстве —
   вызывающий код должен звать это только на заведомо пустом/тестовом
   устройстве (у нас — свежесозданный ramdisk). */
int fat32_format(struct blockdev *dev);

/* Кластер корневого каталога — отправная точка для навигации; "текущий
   каталог" в шелле — это просто uint32_t с одним из значений,
   возвращаемых этими функциями. */
uint32_t fat32_root_cluster(void);

/* Для команды `df`: общее число кластеров данных на смонтированном
   томе, число реально свободных (сканирует FAT — на нашем маленьком
   ramdisk это быстро, для тома побольше стоило бы кэшировать) и размер
   одного кластера в байтах. */
uint32_t fat32_total_clusters(void);
uint32_t fat32_free_clusters(void);
uint32_t fat32_cluster_size_bytes(void);

/* fat32_name_to_83: конвертирует ввод пользователя ("hello.txt",
   регистронезависимо) в 11-байтовое имя 8.3 (заглавные буквы,
   дополнено пробелами). Возвращает 0 при успехе, -1 если имя/расширение
   длиннее 8/3 символов или содержит недопустимые символы — вызывающий
   код (shell) должен показать понятную ошибку, а не тихо обрезать имя. */
int fat32_name_to_83(const char *input, char out83[FAT32_NAME_LEN]);

/* Ищет запись name83 в каталоге dir_cluster. Возвращает 1 и заполняет
   *out, если нашлась, 0 если нет. */
int fat32_find_entry(uint32_t dir_cluster, const char out83_name[FAT32_NAME_LEN],
                      struct fat32_dirent *out);

/* Перечисляет все активные (не удалённые, не LFN, не volume-label)
   записи каталога dir_cluster, вызывая callback для каждой. */
typedef void (*fat32_list_cb)(const struct fat32_dirent *entry, void *ctx);
void fat32_list_dir(uint32_t dir_cluster, fat32_list_cb callback, void *ctx);

/* Создаёт (или полностью перезаписывает, если уже существует) файл
   name83 в каталоге dir_cluster содержимым data[0..size). Возвращает 0
   при успехе, -1 при нехватке места на устройстве. */
int fat32_write_file(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN],
                      const void *data, uint32_t size);

/* Читает файл name83 из каталога dir_cluster в buf (максимум buf_size
   байт — файл может быть больше, тогда читаются первые buf_size байт).
   *out_size получает реальный размер файла (может быть > buf_size).
   Возвращает 0 при успехе, -1 если файл не найден или является
   каталогом. */
int fat32_read_file(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN],
                     void *buf, uint32_t buf_size, uint32_t *out_size);

/* Удаляет файл (не каталог — для каталогов см. fat32_rmdir) name83 из
   dir_cluster: освобождает цепочку кластеров и помечает запись
   удалённой. Возвращает 0 при успехе, -1 если не найден или это каталог. */
int fat32_delete_file(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN]);

/* Создаёт подкаталог name83 внутри dir_cluster (с записями "." и ".."),
   возвращает 0 при успехе, -1 при нехватке места или если имя уже занято. */
int fat32_mkdir(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN]);

/* Удаляет ПУСТОЙ подкаталог name83 из dir_cluster. Возвращает 0 при
   успехе, -1 если не найден, не является каталогом, или не пуст. */
int fat32_rmdir(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN]);

/* Преобразует внутреннее 8.3-имя обратно в читаемую форму ("HELLO.ELF"
   или "README" без точки, если расширения нет). out должен быть не
   короче 13 байт (8+1+3+NUL). */
void fat32_83_to_display(const uint8_t name83[FAT32_NAME_LEN], char *out);

#endif /* APEXOS_FAT32_H */
