#include "apxp.h"
#include "fat32.h"
#include "net.h"
#include "serial.h"
#include "console.h"
#include "string.h"
#include "kheap.h"

static void shell_write_safe(const char *s) {
    console_write(s);
    serial_write(s);
}

static void shell_printf_safe(const char *fmt, ...) {
    va_list a1, a2;
    va_start(a1, fmt);
    va_copy(a2, a1);
    serial_vprintf(fmt, a1);
    console_vprintf(fmt, a2);
    va_end(a1);
    va_end(a2);
}

static int apxp_execute_line(const char *line) {
    char buf[256];
    strcpy_safe(buf, sizeof(buf), line);

    char *argv[16];
    int argc = 0;
    char *p = buf;
    while (*p != '\0' && argc < 16) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ') p++;
        if (*p == ' ') { *p = '\0'; p++; }
    }

    if (argc == 0) return 0;

    if (strcmp(argv[0], "exit") == 0) {
        return 1;
    }
    if (strcmp(argv[0], "dlw") == 0) {
        if (argc < 2) {
            shell_write_safe("dlw: usage: dlw <url>\n");
            return 0;
        }
        const char *url = argv[1];
        char host[64] = "raw.githubusercontent.com";
        char path[192] = "/kataevilya/apexosprograms/master/";

        const char *ap = strstr(url, ".apxp");
        if (ap) {
            size_t host_len = (size_t)(ap - url);
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            memcpy(host, url, host_len);
            host[host_len] = '\0';
            const char *slash = strrchr(host, '/');
            if (slash) {
                size_t off = (size_t)(slash - host);
                size_t path_len = host_len - off;
                memcpy(path, slash, path_len);
                path[path_len] = '\0';
            }
        } else {
            size_t url_len = strlen(url);
            if (url_len >= sizeof(path)) url_len = sizeof(path) - 1;
            memcpy(path, url, url_len);
            path[url_len] = '\0';
        }

        char dlbuf[64 * 1024];
        uint32_t out_len = 0;
        if (net_http_get(host, path, dlbuf, sizeof(dlbuf) - 1, &out_len) == 0) {
            const char *fname = strrchr(path, '/');
            if (!fname) fname = path;
            else fname++;

            char name83[FAT32_NAME_LEN];
            if (fat32_name_to_83(fname, name83) != 0) {
                shell_write_safe("dlw: invalid package name (8.3 names only)\n");
                return 0;
            }

            if (fat32_write_file(fat32_root_cluster(), name83, dlbuf, out_len) != 0) {
                shell_write_safe("dlw: failed to save package (no space?)\n");
            } else {
                shell_printf_safe("dlw: installed %s (%u bytes)\n", fname, (unsigned)out_len);
            }
        } else {
            shell_write_safe("dlw: download failed\n");
        }
        return 0;
    }
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            console_write(argv[i]);
            serial_write(argv[i]);
            if (i + 1 < argc) { console_write(" "); serial_write(" "); }
        }
        console_putc('\n');
        serial_putc('\n');
        return 0;
    }
    if (strcmp(argv[0], "help") == 0) {
        shell_write_safe("APXP commands: exit, dlw <url>, echo <text>, help\n");
        return 0;
    }
    shell_printf_safe("apxp: unknown command: %s\n", argv[0]);
    return 0;
}

int apxp_run(const char *path) {
    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(path, name83) != 0) {
        shell_write_safe("apxp: invalid package name\n");
        return -1;
    }

    char *buf = (char *)kmalloc(64 * 1024);
    if (buf == NULL) {
        shell_write_safe("apxp: out of memory\n");
        return -1;
    }

    uint32_t real_size = 0;
    if (fat32_read_file(fat32_root_cluster(), name83, buf, 64 * 1024, &real_size) != 0) {
        shell_write_safe("apxp: package not found\n");
        kfree(buf);
        return -1;
    }

    if (!apxp_validate(buf, real_size)) {
        shell_write_safe("apxp: invalid package format\n");
        kfree(buf);
        return -1;
    }

    shell_printf_safe("apxp: running %s\n", path);

    char manifest[256];
    memcpy(manifest, buf + sizeof(struct apxp_header), sizeof(manifest) - 1);
    manifest[sizeof(manifest) - 1] = '\0';

    char *p = manifest;
    while (*p != '\0') {
        while (*p == '\n' || *p == '\r') p++;
        if (*p == '\0') break;
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        if (apxp_execute_line(p)) {
            kfree(buf);
            return 0;
        }
        if (eol) p = eol + 1;
        else break;
    }

    kfree(buf);
    return 0;
}

int apxp_validate(const void *data, size_t len) {
    if (len < sizeof(struct apxp_header)) return 0;
    const struct apxp_header *h = (const struct apxp_header *)data;
    if (h->magic != APXP_MAGIC) return 0;
    if (h->version != APXP_VERSION) return 0;
    size_t total = sizeof(struct apxp_header) + h->manifest_size + h->payload_size;
    if (total > len) return 0;
    return 1;
}

int apxp_install(const void *data, size_t len) {
    if (!apxp_validate(data, len)) return -1;
    const struct apxp_header *h = (const struct apxp_header *)data;
    const char *fname = (const char *)data + sizeof(struct apxp_header) + h->manifest_size;
    if (fname[0] == '\0') return -1;

    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(fname, name83) != 0) return -1;

    const void *payload = (const char *)data + sizeof(struct apxp_header) + h->manifest_size;
    if (fat32_write_file(fat32_root_cluster(), name83, payload, h->payload_size) != 0) {
        return -1;
    }
    return 0;
}

void apxp_init(void) {
    serial_write("[apxp] package manager initialized\n");
}
