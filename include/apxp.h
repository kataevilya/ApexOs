#ifndef APEXOS_APXP_H
#define APEXOS_APXP_H

#include <stdint.h>
#include <stddef.h>

#define APXP_MAGIC 0x41585058
#define APXP_VERSION 1

struct apxp_header {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t manifest_size;
    uint32_t payload_size;
    uint32_t checksum;
} __attribute__((packed));

int  apxp_validate(const void *data, size_t len);
int  apxp_install(const void *data, size_t len);
int  apxp_run(const char *path);
int  pkgmgr_download(const char *url);
void apxp_init(void);

#endif /* APEXOS_APXP_H */
