#include "apxp.h"
#include "fat32.h"
#include "net.h"
#include "serial.h"
#include "console.h"
#include "string.h"
#include "kheap.h"

int pkgmgr_download(const char *url) {
    char host[64] = {0};
    char path[192] = {0};

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    const char *slash = strchr(p, '/');
    if (slash) {
        size_t host_len = (size_t)(slash - p);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        memcpy(host, p, host_len);
        size_t path_len = strlen(slash);
        if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
        memcpy(path, slash, path_len);
        path[path_len] = '\0';
    } else {
        size_t host_len = strlen(p);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        memcpy(host, p, host_len);
        strcpy(path, "/");
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
