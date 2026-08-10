#include "apxp.h"
#include "fat32.h"
#include "net.h"
#include "serial.h"
#include "console.h"
#include "string.h"
#include "kheap.h"

#define DEFAULT_PKG_HOST "raw.githubusercontent.com"
#define DEFAULT_PKG_PATH "/kataevilya/apexosprograms/main/"

int pkgmgr_download(const char *url) {
    char host[64] = {0};
    char path[192] = {0};

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    int has_host = 0;
    const char *slash = strchr(p, '/');
    if (slash && slash != p) {
        size_t host_len = (size_t)(slash - p);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        memcpy(host, p, host_len);
        size_t path_len = strlen(slash);
        if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
        memcpy(path, slash, path_len);
        path[path_len] = '\0';
        has_host = 1;
    } else if (slash == NULL && *p != '\0') {
        const char *dot = strchr(p, '.');
        if (dot && strcmp(dot, ".apxp") == 0) {
            has_host = 0;
        } else {
            size_t host_len = strlen(p);
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            memcpy(host, p, host_len);
            strcpy(path, "/");
            has_host = 1;
        }
    }

    if (!has_host) {
        strcpy(host, DEFAULT_PKG_HOST);
        strcpy(path, DEFAULT_PKG_PATH);
        size_t url_len = strlen(url);
        if (url_len > 0 && url[0] == '/') {
            size_t path_len = strlen(url);
            if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
            memcpy(path, url, path_len);
            path[path_len] = '\0';
        } else if (url_len > 0) {
            size_t path_len = strlen(DEFAULT_PKG_PATH) + url_len;
            if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
            memcpy(path, DEFAULT_PKG_PATH, strlen(DEFAULT_PKG_PATH));
            memcpy(path + strlen(DEFAULT_PKG_PATH), url, url_len);
            path[path_len] = '\0';
        }
    }

    if (host[0] == '\0') {
        serial_write("[pkgmgr] invalid url: no host\n");
        return -1;
    }

    char dlbuf[64 * 1024];
    uint32_t out_len = 0;
    if (net_http_get(host, path, dlbuf, sizeof(dlbuf) - 1, &out_len) != 0) {
        serial_printf("[pkgmgr] download failed for %s\n", url);
        return -1;
    }

    const char *fname = strrchr(path, '/');
    if (!fname) fname = path;
    else fname++;

    char name83[FAT32_NAME_LEN];
    if (fat32_name_to_83(fname, name83) != 0) {
        serial_printf("[pkgmgr] invalid filename: %s\n", fname);
        return -1;
    }

    if (fat32_write_file(fat32_root_cluster(), name83, dlbuf, out_len) != 0) {
        serial_printf("[pkgmgr] failed to save %s (no space?)\n", fname);
        return -1;
    }

    serial_printf("[pkgmgr] installed %s (%u bytes)\n", fname, (unsigned)out_len);
    return 0;
}
