#include "net.h"
#include "rtl8139.h"
#include "serial.h"
#include "console.h"
#include "string.h"
#include "pit.h"
#include <stdint.h>

#define DNS_PORT 53

static dns_callback_t g_dns_cb = NULL;
static uint16_t g_dns_tid = 0;
static uint32_t g_dns_result = 0;
static int g_dns_done = 0;

void dns_set_callback(dns_callback_t cb) {
    g_dns_cb = cb;
}

static void dns_handle(uint16_t sport, const void *buf, size_t len) {
    if (sport != DNS_PORT || g_dns_cb == NULL || len < 12) return;
    const uint8_t *p = (const uint8_t *)buf;
    uint16_t tid = ((uint16_t)p[0] << 8) | p[1];
    if (tid != g_dns_tid) return;

    uint16_t ancount = ((uint16_t)p[6] << 8) | p[7];
    if (ancount < 1) {
        console_write("[dns] no answer\n");
        serial_write("[dns] no answer\n");
        g_dns_cb(0);
        return;
    }

    const uint8_t *answer = p + 12;
    while (answer < (const uint8_t *)buf + len && *answer != 0) {
        uint8_t l = *answer++;
        if ((l & 0xC0) == 0xC0) { answer += 1; continue; }
        answer += l;
    }
    answer += 5;
    if (answer + 10 > (const uint8_t *)buf + len) {
        console_write("[dns] bad answer\n");
        serial_write("[dns] bad answer\n");
        g_dns_cb(0);
        return;
    }
    answer += 10;
    uint16_t rdlen = ((uint16_t)answer[0] << 8) | answer[1];
    answer += 2;
    if (rdlen != 4 || answer + 4 > (const uint8_t *)buf + len) {
        console_write("[dns] bad rdlen\n");
        serial_write("[dns] bad rdlen\n");
        g_dns_cb(0);
        return;
    }
    uint32_t ip = ((uint32_t)answer[0] << 24) | ((uint32_t)answer[1] << 16) |
                  ((uint32_t)answer[2] << 8) | answer[3];
    console_printf("[dns] resolved to %u.%u.%u.%u\n",
                   (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                   (ip >> 8) & 0xFF, ip & 0xFF);
    serial_printf("[dns] resolved to %u.%u.%u.%u\n",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                  (ip >> 8) & 0xFF, ip & 0xFF);
    g_dns_cb(ip);
}

static void dns_cb_wrapper(uint32_t ip) {
    g_dns_result = ip;
    g_dns_done = 1;
}

int net_dns_resolve(const char *hostname, uint32_t *out_ip) {
    console_printf("[dns] resolving %s\n", hostname);
    serial_printf("[dns] resolving %s\n", hostname);
    g_dns_tid = (uint16_t)((uintptr_t)hostname & 0xFFFF);
    if (g_dns_tid == 0) g_dns_tid = 0x1234;

    g_dns_result = 0;
    g_dns_done = 0;

    uint8_t pkt[256];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (g_dns_tid >> 8) & 0xFF;
    pkt[1] = g_dns_tid & 0xFF;
    pkt[2] = 0x01; pkt[3] = 0x00;
    pkt[4] = 0x00; pkt[5] = 0x01;
    pkt[6] = 0x00; pkt[7] = 0x00;
    pkt[8] = 0x00; pkt[9] = 0x00;
    pkt[10] = 0x00; pkt[11] = 0x00;

    size_t pos = 12;
    const char *dot = hostname;
    while (*dot) {
        const char *next = dot;
        while (*next && *next != '.') next++;
        size_t seglen = (size_t)(next - dot);
        pkt[pos++] = (uint8_t)seglen;
        for (size_t i = 0; i < seglen; i++) pkt[pos++] = (uint8_t)dot[i];
        if (*next == '.') dot = next + 1;
        else dot = next;
    }
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x01;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;

    dns_set_callback(dns_cb_wrapper);
    udp_set_callback(dns_handle, 53);
    udp_send(53, g_net.dns_server, pkt, pos);

    uint32_t deadline = pit_get_ticks() + 200;
    while (!g_dns_done && pit_get_ticks() < deadline) {
        net_tx_poll();
        for (volatile int i = 0; i < 50000; i++) __asm__ volatile ("nop");
    }

    if (!g_dns_done) {
        console_write("[dns] timeout\n");
        serial_write("[dns] timeout\n");
    }

    if (out_ip) *out_ip = g_dns_result;

    return g_dns_result != 0 ? 0 : -1;
}
