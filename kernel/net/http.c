#include "net.h"
#include "rtl8139.h"
#include "serial.h"
#include "string.h"
#include "pit.h"
#include <stdint.h>

size_t string_concat(char *dst, size_t dst_size, const char *a, const char *b, const char *c) {
    size_t pos = 0;
    if (a && pos < dst_size) {
        size_t la = strlen(a);
        if (la > dst_size - pos) la = dst_size - pos;
        memcpy(dst + pos, a, la);
        pos += la;
    }
    if (b && pos < dst_size) {
        size_t lb = strlen(b);
        if (lb > dst_size - pos) lb = dst_size - pos;
        memcpy(dst + pos, b, lb);
        pos += lb;
    }
    if (c && pos < dst_size) {
        size_t lc = strlen(c);
        if (lc > dst_size - pos) lc = dst_size - pos;
        memcpy(dst + pos, c, lc);
        pos += lc;
    }
    if (pos < dst_size) dst[pos] = '\0';
    return pos;
}

int net_http_get(const char *host, const char *path, char *buf, size_t max_len, uint32_t *out_len) {
    uint32_t server_ip = 0;
    if (net_dns_resolve(host, &server_ip) != 0) {
        serial_printf("[http] dns failed for %s\n", host);
        return -1;
    }

    char req[512];
    size_t reqlen = 0;
    reqlen += string_concat(req, sizeof(req) - reqlen, "GET ", path, " HTTP/1.0\r\n");
    reqlen += string_concat(req, sizeof(req) - reqlen, "Host: ", host, "\r\n");
    reqlen += string_concat(req, sizeof(req) - reqlen, "User-Agent: ApexOS/0.1\r\n", NULL, NULL);
    reqlen += string_concat(req, sizeof(req) - reqlen, "Connection: close\r\n\r\n", NULL, NULL);

    if (net_tcp_connect(server_ip, 80) != 0) {
        serial_printf("[http] tcp connect to %s failed\n", host);
        return -1;
    }

    net_tcp_send(req, reqlen);

    size_t total = 0;
    uint32_t deadline = pit_get_ticks() + 100;
    while (total < max_len - 1) {
        if (pit_get_ticks() > deadline) break;
        int got = net_tcp_recv(buf + total, max_len - 1 - total);
        if (got > 0) {
            total += (size_t)got;
            deadline = pit_get_ticks() + 100;
        } else {
            for (volatile int i = 0; i < 50000; i++) __asm__ volatile ("nop");
        }
    }

    buf[total] = '\0';
    if (out_len) *out_len = (uint32_t)total;

    net_tcp_close();
    serial_printf("[http] downloaded %u bytes from %s\n", (unsigned)total, host);
    return 0;
}
