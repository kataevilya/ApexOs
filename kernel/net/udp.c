#include "net.h"
#include "serial.h"
#include "string.h"

void ip_send(uint8_t proto, uint32_t dst, const void *payload, uint16_t plen);
static udp_callback_t g_udp_cb = NULL;
static uint16_t g_udp_port = 0;

void udp_set_callback(udp_callback_t cb, uint16_t port) {
    g_udp_cb = cb;
    g_udp_port = port;
}

void udp_handle(const void *buf, size_t len) {
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *uh = (const struct udp_hdr *)buf;
    uint16_t dport = ((uint16_t)uh->dport[0] << 8) | uh->dport[1];
    if (g_udp_cb && dport == g_udp_port) {
        const uint8_t *payload = (const uint8_t *)buf + sizeof(struct udp_hdr);
        size_t plen = len - sizeof(struct udp_hdr);
        g_udp_cb(((uint16_t)uh->sport[0] << 8) | uh->sport[1], payload, plen);
    }
}

void udp_send(uint16_t dst_port, uint32_t ip, const void *payload, uint16_t plen) {
    uint8_t frame[2048];
    struct udp_hdr uh;
    uh.sport[0] = (g_udp_port >> 8) & 0xFF;
    uh.sport[1] = g_udp_port & 0xFF;
    uh.dport[0] = (dst_port >> 8) & 0xFF;
    uh.dport[1] = dst_port & 0xFF;
    uh.len[0] = (sizeof(struct udp_hdr) + plen) >> 8;
    uh.len[1] = (sizeof(struct udp_hdr) + plen) & 0xFF;
    uh.csum[0] = 0; uh.csum[1] = 0;

    struct ip_pseudo_hdr ph;
    for (int i = 0; i < 4; i++) {
        ph.src[i] = (g_net.ip_addr >> (24 - i * 8)) & 0xFF;
        ph.dst[i] = (ip >> (24 - i * 8)) & 0xFF;
    }
    ph.zero = 0;
    ph.proto = IP_PROTO_UDP;
    ph.len[0] = uh.len[0];
    ph.len[1] = uh.len[1];

    uint16_t csum_val = net_csum(&ph, sizeof(ph));
    csum_val = net_csum_combine(csum_val, &uh, sizeof(uh));
    csum_val = net_csum_combine(csum_val, payload, plen);
    uh.csum[0] = (csum_val >> 8) & 0xFF;
    uh.csum[1] = csum_val & 0xFF;

    memcpy(frame, &uh, sizeof(uh));
    memcpy(frame + sizeof(uh), payload, plen);
    ip_send(IP_PROTO_UDP, ip, frame, sizeof(uh) + plen);
}
