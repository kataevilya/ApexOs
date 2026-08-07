#include "fat32.h"
#include "panic.h"
#include "serial.h"
#include "string.h"

/* Компилируемый предел размера кластера (байт) — наш formatter всегда
   использует 4 KiB кластеры (см. fat32_format), это просто защитный
   верхний предел для стековых буферов, а не runtime-переменная. */
#define FAT32_MAX_CLUSTER_BYTES 4096

#define FAT32_EOC_MIN 0x0FFFFFF8u /* >= это значение — конец цепочки кластеров */
#define FAT32_CLUSTER_MASK 0x0FFFFFFFu /* верхние 4 бита каждой FAT-записи зарезервированы */

struct fat32_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} __attribute__((packed));

struct fat32_fsinfo {
    uint32_t lead_signature;
    uint8_t  reserved1[480];
    uint32_t struct_signature;
    uint32_t free_count;
    uint32_t next_free;
    uint8_t  reserved2[12];
    uint32_t trail_signature;
} __attribute__((packed));

static struct blockdev *g_dev = NULL;
static uint32_t g_bytes_per_sector = 0;
static uint32_t g_sectors_per_cluster = 0;
static uint32_t g_reserved_sectors = 0;
static uint32_t g_num_fats = 0;
static uint32_t g_fat_size = 0;
static uint32_t g_root_cluster = 0;
static uint32_t g_first_data_sector = 0;
static uint32_t g_total_clusters = 0;
static uint32_t g_cluster_size_bytes = 0;
static int g_mounted = 0;

uint32_t fat32_root_cluster(void) { return g_root_cluster; }

uint32_t fat32_total_clusters(void) { return g_total_clusters; }
uint32_t fat32_cluster_size_bytes(void) { return g_cluster_size_bytes; }

static inline uint32_t cluster_to_lba(uint32_t cluster) {
    return g_first_data_sector + (cluster - 2) * g_sectors_per_cluster;
}

static void read_cluster(uint32_t cluster, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t lba = cluster_to_lba(cluster);
    for (uint32_t i = 0; i < g_sectors_per_cluster; i++) {
        if (g_dev->read_sector(g_dev, lba + i, p + (uint64_t)i * g_bytes_per_sector) != 0) {
            panic("fat32: read_sector failed at lba %u (cluster %u) — device error mid-operation "
                  "leaves the volume in an unknown state, cannot safely continue",
                  lba + i, cluster);
        }
    }
}

static void write_cluster(uint32_t cluster, const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t lba = cluster_to_lba(cluster);
    for (uint32_t i = 0; i < g_sectors_per_cluster; i++) {
        if (g_dev->write_sector(g_dev, lba + i, p + (uint64_t)i * g_bytes_per_sector) != 0) {
            panic("fat32: write_sector failed at lba %u (cluster %u)", lba + i, cluster);
        }
    }
}

static uint32_t fat_get(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_reserved_sectors + fat_offset / g_bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % g_bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    if (g_dev->read_sector(g_dev, fat_sector, sector) != 0) {
        panic("fat32: fat_get: read_sector failed at FAT sector %u", fat_sector);
    }
    uint32_t value;
    memcpy(&value, sector + offset_in_sector, 4);
    return value & FAT32_CLUSTER_MASK;
}

static void fat_set(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t offset_in_sector = fat_offset % g_bytes_per_sector;
    value &= FAT32_CLUSTER_MASK;

    for (uint32_t copy = 0; copy < g_num_fats; copy++) {
        uint32_t fat_sector = g_reserved_sectors + copy * g_fat_size + fat_offset / g_bytes_per_sector;
        uint8_t sector[SECTOR_SIZE];
        if (g_dev->read_sector(g_dev, fat_sector, sector) != 0) {
            panic("fat32: fat_set: read_sector failed at FAT sector %u", fat_sector);
        }
        memcpy(sector + offset_in_sector, &value, 4);
        if (g_dev->write_sector(g_dev, fat_sector, sector) != 0) {
            panic("fat32: fat_set: write_sector failed at FAT sector %u", fat_sector);
        }
    }
}

uint32_t fat32_free_clusters(void) {
    /* Линейный скан FAT — как и alloc_cluster(), осознанно просто.
       На нашем ramdisk (~2000 кластеров) это доли миллисекунды; для
       тома побольше стоило бы держать счётчик, обновляемый в
       alloc_cluster()/free_chain(), а не пересчитывать каждый раз. */
    uint32_t free_count = 0;
    for (uint32_t c = 2; c < g_total_clusters + 2; c++) {
        if (fat_get(c) == 0) {
            free_count++;
        }
    }
    return free_count;
}

/* alloc_cluster: линейный скан FAT от кластера 2. Как и PMM — просто,
   осознанно не оптимизировано (кэш "следующего свободного" — будущая
   работа, не здесь). Возвращает 0, если свободных кластеров нет. */
static uint32_t alloc_cluster(void) {
    for (uint32_t c = 2; c < g_total_clusters + 2; c++) {
        if (fat_get(c) == 0) {
            fat_set(c, FAT32_EOC_MIN | 0x7); /* EOC-маркер (0x0FFFFFFF после маскирования) */
            return c;
        }
    }
    return 0;
}

static void free_chain(uint32_t start) {
    uint32_t cluster = start;
    uint32_t guard = 0;
    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        if (guard++ > g_total_clusters + 2) {
            panic("fat32: free_chain: cluster chain longer than total_clusters — "
                  "corrupted FAT (cycle?), refusing to loop forever");
        }
        uint32_t next = fat_get(cluster);
        fat_set(cluster, 0);
        cluster = next;
    }
}

/* find_entry_location: единственное место, где реально идёт поиск по
   каталогу — find_entry/write/delete/mkdir/rmdir все используют её
   (через это, не дублируя обход цепочки кластеров в пяти местах). */
static int find_entry_location(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN],
                                uint32_t *out_cluster, uint32_t *out_index,
                                struct fat32_dirent *out_entry) {
    uint8_t buf[FAT32_MAX_CLUSTER_BYTES];
    uint32_t entries_per_cluster = g_cluster_size_bytes / sizeof(struct fat32_dirent);
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        read_cluster(cluster, buf);
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat32_dirent *e = (struct fat32_dirent *)(buf + i * sizeof(struct fat32_dirent));
            if (e->name[0] == 0x00) {
                return 0; /* конец записей во всём каталоге */
            }
            if ((uint8_t)e->name[0] == 0xE5 || e->attr == FAT32_ATTR_LFN ||
                (e->attr & FAT32_ATTR_VOLUME_ID)) {
                continue;
            }
            if (memcmp(e->name, name83, FAT32_NAME_LEN) == 0) {
                if (out_cluster) *out_cluster = cluster;
                if (out_index) *out_index = i;
                if (out_entry) *out_entry = *e;
                return 1;
            }
        }
        cluster = fat_get(cluster);
        if (guard++ > g_total_clusters + 2) {
            panic("fat32: find_entry_location: corrupted directory FAT chain (cycle?)");
        }
    }
    return 0;
}

int fat32_find_entry(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN], struct fat32_dirent *out) {
    return find_entry_location(dir_cluster, name83, NULL, NULL, out);
}

void fat32_list_dir(uint32_t dir_cluster, fat32_list_cb callback, void *ctx) {
    uint8_t buf[FAT32_MAX_CLUSTER_BYTES];
    uint32_t entries_per_cluster = g_cluster_size_bytes / sizeof(struct fat32_dirent);
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        read_cluster(cluster, buf);
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat32_dirent *e = (struct fat32_dirent *)(buf + i * sizeof(struct fat32_dirent));
            if (e->name[0] == 0x00) {
                return;
            }
            if ((uint8_t)e->name[0] == 0xE5 || e->attr == FAT32_ATTR_LFN ||
                (e->attr & FAT32_ATTR_VOLUME_ID)) {
                continue;
            }
            callback(e, ctx);
        }
        cluster = fat_get(cluster);
        if (guard++ > g_total_clusters + 2) {
            panic("fat32: fat32_list_dir: corrupted directory FAT chain (cycle?)");
        }
    }
}

/* create_entry: находит свободный слот (0x00 или 0xE5) в цепочке
   dir_cluster, расширяя каталог новым кластером, если все существующие
   слоты заняты активными записями. */
static int create_entry(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN],
                         uint8_t attr, uint32_t first_cluster, uint32_t file_size) {
    uint8_t buf[FAT32_MAX_CLUSTER_BYTES];
    uint32_t entries_per_cluster = g_cluster_size_bytes / sizeof(struct fat32_dirent);
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0;
    uint32_t last_cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        last_cluster = cluster;
        read_cluster(cluster, buf);
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat32_dirent *e = (struct fat32_dirent *)(buf + i * sizeof(struct fat32_dirent));
            if (e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5) {
                memset(e, 0, sizeof(struct fat32_dirent));
                memcpy(e->name, name83, FAT32_NAME_LEN);
                e->attr = attr;
                e->first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
                e->first_cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                e->file_size = file_size;
                write_cluster(cluster, buf);
                return 0;
            }
        }
        cluster = fat_get(cluster);
        if (guard++ > g_total_clusters + 2) {
            panic("fat32: create_entry: corrupted directory FAT chain (cycle?)");
        }
    }

    /* Каталог целиком занят активными записями — расширяем новым кластером. */
    uint32_t new_cluster = alloc_cluster();
    if (new_cluster == 0) {
        return -1; /* нет места на устройстве — не panic, штатная нехватка ресурса */
    }
    memset(buf, 0, g_cluster_size_bytes);
    struct fat32_dirent *e = (struct fat32_dirent *)buf;
    memcpy(e->name, name83, FAT32_NAME_LEN);
    e->attr = attr;
    e->first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    e->first_cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
    e->file_size = file_size;
    write_cluster(new_cluster, buf);

    fat_set(last_cluster, new_cluster);
    return 0;
}

int fat32_write_file(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN],
                      const void *data, uint32_t size) {
    uint32_t existing_cluster_loc = 0, existing_index = 0;
    struct fat32_dirent existing;
    int exists = find_entry_location(dir_cluster, name83, &existing_cluster_loc, &existing_index, &existing);
    if (exists && (existing.attr & FAT32_ATTR_DIRECTORY)) {
        return -1; /* нельзя перезаписать каталог как файл */
    }
    if (exists) {
        uint32_t old_first = ((uint32_t)existing.first_cluster_high << 16) | existing.first_cluster_low;
        if (old_first != 0) {
            free_chain(old_first);
        }
    }

    uint32_t clusters_needed = (size == 0) ? 0 : (size + g_cluster_size_bytes - 1) / g_cluster_size_bytes;
    uint32_t new_first = 0;
    uint32_t prev = 0;
    uint32_t written = 0;
    uint8_t cluster_buf[FAT32_MAX_CLUSTER_BYTES];

    for (uint32_t i = 0; i < clusters_needed; i++) {
        uint32_t c = alloc_cluster();
        if (c == 0) {
            if (new_first != 0) {
                free_chain(new_first); /* не оставляем повисшую частичную цепочку при нехватке места */
            }
            return -1;
        }
        if (prev == 0) {
            new_first = c;
        } else {
            fat_set(prev, c);
        }
        prev = c;

        uint32_t chunk = size - written;
        if (chunk > g_cluster_size_bytes) {
            chunk = g_cluster_size_bytes;
        }
        memset(cluster_buf, 0, g_cluster_size_bytes); /* хвост кластера — нули, не мусор из предыдущего использования */
        memcpy(cluster_buf, (const uint8_t *)data + written, chunk);
        write_cluster(c, cluster_buf);
        written += chunk;
    }

    if (exists) {
        uint8_t dirbuf[FAT32_MAX_CLUSTER_BYTES];
        read_cluster(existing_cluster_loc, dirbuf);
        struct fat32_dirent *e =
            (struct fat32_dirent *)(dirbuf + existing_index * sizeof(struct fat32_dirent));
        e->first_cluster_low = (uint16_t)(new_first & 0xFFFF);
        e->first_cluster_high = (uint16_t)((new_first >> 16) & 0xFFFF);
        e->file_size = size;
        write_cluster(existing_cluster_loc, dirbuf);
    } else {
        if (create_entry(dir_cluster, name83, FAT32_ATTR_ARCHIVE, new_first, size) != 0) {
            if (new_first != 0) {
                free_chain(new_first);
            }
            return -1;
        }
    }
    return 0;
}

int fat32_read_file(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN],
                     void *buf, uint32_t buf_size, uint32_t *out_size) {
    struct fat32_dirent e;
    if (!find_entry_location(dir_cluster, name83, NULL, NULL, &e)) {
        return -1;
    }
    if (e.attr & FAT32_ATTR_DIRECTORY) {
        return -1;
    }

    if (out_size) {
        *out_size = e.file_size;
    }

    uint32_t to_read = (e.file_size < buf_size) ? e.file_size : buf_size;
    uint32_t cluster = ((uint32_t)e.first_cluster_high << 16) | e.first_cluster_low;
    uint32_t read_so_far = 0;
    uint8_t cluster_buf[FAT32_MAX_CLUSTER_BYTES];
    uint32_t guard = 0;

    while (read_so_far < to_read && cluster >= 2 && cluster < FAT32_EOC_MIN) {
        read_cluster(cluster, cluster_buf);
        uint32_t chunk = to_read - read_so_far;
        if (chunk > g_cluster_size_bytes) {
            chunk = g_cluster_size_bytes;
        }
        memcpy((uint8_t *)buf + read_so_far, cluster_buf, chunk);
        read_so_far += chunk;
        cluster = fat_get(cluster);
        if (guard++ > g_total_clusters + 2) {
            panic("fat32: fat32_read_file: corrupted FAT chain (cycle?)");
        }
    }
    return 0;
}

int fat32_delete_file(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN]) {
    uint32_t loc_cluster, loc_index;
    struct fat32_dirent e;
    if (!find_entry_location(dir_cluster, name83, &loc_cluster, &loc_index, &e)) {
        return -1;
    }
    if (e.attr & FAT32_ATTR_DIRECTORY) {
        return -1; /* для каталогов — fat32_rmdir */
    }

    uint32_t first = ((uint32_t)e.first_cluster_high << 16) | e.first_cluster_low;
    if (first != 0) {
        free_chain(first);
    }

    uint8_t dirbuf[FAT32_MAX_CLUSTER_BYTES];
    read_cluster(loc_cluster, dirbuf);
    dirbuf[loc_index * sizeof(struct fat32_dirent)] = 0xE5;
    write_cluster(loc_cluster, dirbuf);
    return 0;
}

static int dir_is_empty(uint32_t dir_cluster) {
    uint8_t buf[FAT32_MAX_CLUSTER_BYTES];
    uint32_t entries_per_cluster = g_cluster_size_bytes / sizeof(struct fat32_dirent);
    uint32_t cluster = dir_cluster;
    uint32_t guard = 0;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        read_cluster(cluster, buf);
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            struct fat32_dirent *e = (struct fat32_dirent *)(buf + i * sizeof(struct fat32_dirent));
            if (e->name[0] == 0x00) {
                return 1;
            }
            if ((uint8_t)e->name[0] == 0xE5 || e->attr == FAT32_ATTR_LFN) {
                continue;
            }
            int is_dot = (e->name[0] == '.' && e->name[1] == ' ');
            int is_dotdot = (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == ' ');
            if (!is_dot && !is_dotdot) {
                return 0;
            }
        }
        cluster = fat_get(cluster);
        if (guard++ > g_total_clusters + 2) {
            panic("fat32: dir_is_empty: corrupted FAT chain (cycle?)");
        }
    }
    return 1;
}

int fat32_mkdir(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN]) {
    if (find_entry_location(dir_cluster, name83, NULL, NULL, NULL)) {
        return -1; /* уже существует (файл или каталог) */
    }

    uint32_t new_cluster = alloc_cluster();
    if (new_cluster == 0) {
        return -1;
    }

    uint8_t buf[FAT32_MAX_CLUSTER_BYTES];
    memset(buf, 0, g_cluster_size_bytes);

    struct fat32_dirent *dot = (struct fat32_dirent *)buf;
    memset(dot->name, ' ', FAT32_NAME_LEN);
    dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->first_cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    dot->first_cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);

    struct fat32_dirent *dotdot = (struct fat32_dirent *)(buf + sizeof(struct fat32_dirent));
    memset(dotdot->name, ' ', FAT32_NAME_LEN);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    /* ".." корневого подкаталога указывает на 0 (спецзначение "это корень"),
       а не на реальный номер кластера корня — так того требует спецификация. */
    uint32_t parent_ref = (dir_cluster == g_root_cluster) ? 0 : dir_cluster;
    dotdot->first_cluster_low = (uint16_t)(parent_ref & 0xFFFF);
    dotdot->first_cluster_high = (uint16_t)((parent_ref >> 16) & 0xFFFF);

    write_cluster(new_cluster, buf);

    if (create_entry(dir_cluster, name83, FAT32_ATTR_DIRECTORY, new_cluster, 0) != 0) {
        free_chain(new_cluster);
        return -1;
    }
    return 0;
}

int fat32_rmdir(uint32_t dir_cluster, const char name83[FAT32_NAME_LEN]) {
    uint32_t loc_cluster, loc_index;
    struct fat32_dirent e;
    if (!find_entry_location(dir_cluster, name83, &loc_cluster, &loc_index, &e)) {
        return -1;
    }
    if (!(e.attr & FAT32_ATTR_DIRECTORY)) {
        return -1; /* для файлов — fat32_delete_file */
    }

    uint32_t target = ((uint32_t)e.first_cluster_high << 16) | e.first_cluster_low;
    if (target != 0 && !dir_is_empty(target)) {
        return -1; /* не пуст */
    }
    if (target != 0) {
        free_chain(target);
    }

    uint8_t dirbuf[FAT32_MAX_CLUSTER_BYTES];
    read_cluster(loc_cluster, dirbuf);
    dirbuf[loc_index * sizeof(struct fat32_dirent)] = 0xE5;
    write_cluster(loc_cluster, dirbuf);
    return 0;
}

int fat32_name_to_83(const char *input, char out83[FAT32_NAME_LEN]) {
    for (int i = 0; i < FAT32_NAME_LEN; i++) {
        out83[i] = ' ';
    }

    int name_len = 0, ext_len = 0, in_ext = 0;
    for (const char *p = input; *p; p++) {
        char c = *p;
        if (c == '.') {
            if (in_ext) {
                return -1; /* вторая точка — короткие имена этого не поддерживают */
            }
            in_ext = 1;
            continue;
        }
        char uc = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        int valid = (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || uc == '_' || uc == '-';
        if (!valid) {
            return -1;
        }
        if (!in_ext) {
            if (name_len >= 8) {
                return -1;
            }
            out83[name_len++] = uc;
        } else {
            if (ext_len >= 3) {
                return -1;
            }
            out83[8 + ext_len++] = uc;
        }
    }
    if (name_len == 0) {
        return -1;
    }
    return 0;
}

int fat32_mount(struct blockdev *dev) {
    uint8_t sector[SECTOR_SIZE];
    if (dev->read_sector(dev, 0, sector) != 0) {
        serial_write("[fat32] failed to read boot sector\n");
        return -1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        serial_write("[fat32] boot sector signature 0x55AA missing (not formatted?)\n");
        return -1;
    }

    const struct fat32_bpb *bpb = (const struct fat32_bpb *)sector;
    if (bpb->bytes_per_sector != SECTOR_SIZE) {
        serial_printf("[fat32] unsupported bytes_per_sector=%u (only %u)\n",
                      (unsigned)bpb->bytes_per_sector, (unsigned)SECTOR_SIZE);
        return -1;
    }
    if (bpb->fat_size_32 == 0 || bpb->num_fats == 0 || bpb->sectors_per_cluster == 0) {
        serial_write("[fat32] boot sector fields look invalid (not FAT32?)\n");
        return -1;
    }
    if (memcmp(bpb->fs_type, "FAT32   ", 8) != 0) {
        serial_write("[fat32] fs_type field is not \"FAT32   \"\n");
        return -1;
    }

    uint32_t cluster_size = (uint32_t)bpb->sectors_per_cluster * bpb->bytes_per_sector;
    if (cluster_size > FAT32_MAX_CLUSTER_BYTES) {
        serial_printf("[fat32] cluster size %u exceeds compiled-in max %u\n",
                      cluster_size, (unsigned)FAT32_MAX_CLUSTER_BYTES);
        return -1;
    }

    g_dev = dev;
    g_bytes_per_sector = bpb->bytes_per_sector;
    g_sectors_per_cluster = bpb->sectors_per_cluster;
    g_reserved_sectors = bpb->reserved_sectors;
    g_num_fats = bpb->num_fats;
    g_fat_size = bpb->fat_size_32;
    g_root_cluster = bpb->root_cluster;
    g_first_data_sector = g_reserved_sectors + g_num_fats * g_fat_size;
    g_cluster_size_bytes = cluster_size;

    uint32_t total_sectors = bpb->total_sectors_32;
    if (total_sectors <= g_first_data_sector) {
        serial_write("[fat32] total_sectors_32 too small for declared layout\n");
        return -1;
    }
    g_total_clusters = (total_sectors - g_first_data_sector) / g_sectors_per_cluster;
    g_mounted = 1;

    serial_printf("[fat32] mounted: %u bytes/sector, %u sectors/cluster, %u FAT(s), "
                  "root_cluster=%u, total_clusters=%u\n",
                  g_bytes_per_sector, g_sectors_per_cluster, g_num_fats,
                  g_root_cluster, g_total_clusters);
    return 0;
}

void fat32_83_to_display(const uint8_t name83[FAT32_NAME_LEN], char *out) {
    int pos = 0;
    for (int i = 0; i < 8 && name83[i] != ' '; i++) {
        out[pos++] = name83[i];
    }
    int has_ext = 0;
    for (int i = 8; i < 11; i++) {
        if (name83[i] != ' ') {
            has_ext = 1;
            break;
        }
    }
    if (has_ext) {
        out[pos++] = '.';
        for (int i = 8; i < 11 && name83[i] != ' '; i++) {
            out[pos++] = name83[i];
        }
    }
    out[pos] = '\0';
}

int fat32_format(struct blockdev *dev) {
    if (dev->sector_count > 0xFFFFFFFFull) {
        serial_write("[fat32] device too large for a 32-bit sector count field\n");
        return -1;
    }
    uint32_t total_sectors = (uint32_t)dev->sector_count;

    uint32_t bytes_per_sector = SECTOR_SIZE;
    uint32_t sectors_per_cluster = 8; /* 4 KiB кластеры */
    uint32_t reserved_sectors = 32;
    uint32_t num_fats = 2;

    if (total_sectors <= reserved_sectors) {
        serial_write("[fat32] device too small to format\n");
        return -1;
    }

    /* Стандартная формула размера FAT (Microsoft fatgen103). */
    uint64_t tmp1 = total_sectors - reserved_sectors;
    uint64_t tmp2 = ((uint64_t)256 * sectors_per_cluster + num_fats) / 2;
    uint32_t fat_size = (uint32_t)((tmp1 + tmp2 - 1) / tmp2);

    uint32_t first_data_sector = reserved_sectors + num_fats * fat_size;
    if (first_data_sector >= total_sectors) {
        serial_write("[fat32] device too small to format (FAT area alone exceeds it)\n");
        return -1;
    }
    uint32_t data_sectors = total_sectors - first_data_sector;
    uint32_t total_clusters = data_sectors / sectors_per_cluster;
    uint32_t root_cluster = 2;

    serial_printf("[fat32] formatting: %u sectors total, %u clusters, %u bytes/cluster\n",
                  total_sectors, total_clusters, sectors_per_cluster * bytes_per_sector);
    if (total_clusters < 65525) {
        /* Честная оговорка: официальный порог FAT32 (Microsoft fatgen103) —
           минимум 65525 кластеров, иначе спецификация формально требует
           классифицировать том как FAT16. Наш собственный драйвер это
           не проверяет и работает корректно в любом случае — но строгий
           СТОРОННИЙ FAT32-парсер мог бы отказаться распознавать этот том. */
        serial_write("[fat32] NOTE: cluster count is below the official FAT32 minimum "
                      "(65525) — fine for ApexOS's own driver, but a strict external "
                      "FAT32 implementation might disagree about the volume type\n");
    }

    uint8_t sector[SECTOR_SIZE];
    memset(sector, 0, SECTOR_SIZE);
    sector[0] = 0xEB;
    sector[1] = 0x58;
    sector[2] = 0x90;

    struct fat32_bpb *bpb = (struct fat32_bpb *)sector;
    memset(bpb->oem, ' ', 8);
    memcpy(bpb->oem, "NIKIOS", 6);
    bpb->bytes_per_sector = (uint16_t)bytes_per_sector;
    bpb->sectors_per_cluster = (uint8_t)sectors_per_cluster;
    bpb->reserved_sectors = (uint16_t)reserved_sectors;
    bpb->num_fats = (uint8_t)num_fats;
    bpb->root_entries = 0;
    bpb->total_sectors_16 = 0;
    bpb->media_type = 0xF8;
    bpb->fat_size_16 = 0;
    bpb->sectors_per_track = 32;
    bpb->num_heads = 64;
    bpb->hidden_sectors = 0;
    bpb->total_sectors_32 = total_sectors;
    bpb->fat_size_32 = fat_size;
    bpb->ext_flags = 0;
    bpb->fs_version = 0;
    bpb->root_cluster = root_cluster;
    bpb->fsinfo_sector = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->reserved1 = 0;
    bpb->boot_signature = 0x29;
    bpb->volume_id = 0x4E494B49; /* "NIKI" как magic-метка тома */
    memset(bpb->volume_label, ' ', 11);
    memcpy(bpb->volume_label, "NIKIOS", 6);
    memset(bpb->fs_type, ' ', 8);
    memcpy(bpb->fs_type, "FAT32", 5);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (dev->write_sector(dev, 0, sector) != 0 || dev->write_sector(dev, 6, sector) != 0) {
        serial_write("[fat32] failed writing boot sector (+ backup)\n");
        return -1;
    }

    uint8_t fsinfo_sector[SECTOR_SIZE];
    memset(fsinfo_sector, 0, SECTOR_SIZE);
    struct fat32_fsinfo *fsinfo = (struct fat32_fsinfo *)fsinfo_sector;
    fsinfo->lead_signature = 0x41615252;
    fsinfo->struct_signature = 0x61417272;
    fsinfo->free_count = total_clusters - 1; /* root уже занял кластер 2 */
    fsinfo->next_free = 3;
    fsinfo->trail_signature = 0xAA550000;
    if (dev->write_sector(dev, 1, fsinfo_sector) != 0 || dev->write_sector(dev, 7, fsinfo_sector) != 0) {
        serial_write("[fat32] failed writing FSInfo sector (+ backup)\n");
        return -1;
    }

    uint8_t zero_sector[SECTOR_SIZE];
    memset(zero_sector, 0, SECTOR_SIZE);
    for (uint32_t copy = 0; copy < num_fats; copy++) {
        uint32_t fat_start = reserved_sectors + copy * fat_size;
        for (uint32_t s = 0; s < fat_size; s++) {
            if (dev->write_sector(dev, fat_start + s, zero_sector) != 0) {
                serial_write("[fat32] failed zeroing FAT area\n");
                return -1;
            }
        }
    }

    /* Монтируем то, что только что записали — это заодно честная
       проверка, что fat32_mount() корректно парсит именно то, что
       написал fat32_format() (round-trip самотест). */
    if (fat32_mount(dev) != 0) {
        serial_write("[fat32] format: mounting the freshly-written volume failed (internal bug)\n");
        return -1;
    }

    fat_set(0, 0x0FFFFFF8);
    fat_set(1, 0x0FFFFFFF);
    fat_set(g_root_cluster, 0x0FFFFFFF);

    uint8_t cluster_buf[FAT32_MAX_CLUSTER_BYTES];
    memset(cluster_buf, 0, g_cluster_size_bytes);
    write_cluster(g_root_cluster, cluster_buf);

    serial_write("[fat32] format complete\n");
    return 0;
}
