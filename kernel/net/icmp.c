#include "net.h"
#include "serial.h"
#include "string.h"

void ip_send(uint8_t proto, uint32_t dst, const void *payload, uint16_t plen);
void icmp_handle(const void *buf, size_t len) {
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *ic = (const struct icmp_hdr *)buf;
    uint16_t c = ((uint16_t)ic->csum[0] << 8) | ic->csum[1];
    if (c != 0 && c != net_csum(buf, len)) return;

    if (ic->type == ICMP_ECHO_REQUEST) {
        uint16_t id = ((uint16_t)ic->id[0] << 8) | ic->id[1];
        uint16_t seq = ((uint16_t)ic->seq[0] << 8) | ic->seq[1];
        serial_printf("[icmp] echo request id=%u seq=%u\n", id, seq);

        uint8_t reply[64];
        size_t reply_len = len > sizeof(reply) ? sizeof(reply) : len;
        memcpy(reply, buf, reply_len);
        struct icmp_hdr *r = (struct icmp_hdr *)reply;
        r->type = ICMP_ECHO_REPLY;
        r->csum[0] = 0; r->csum[1] = 0;
        uint16_t csum_val = net_csum(reply, reply_len);
        r->csum[0] = (csum_val >> 8) & 0xFF;
        r->csum[1] = csum_val & 0xFF;

        struct ip_hdr *ip = (struct ip_hdr *)((const uint8_t *)buf - sizeof(struct ip_hdr));
        uint32_t src = ((uint32_t)ip->src[0] << 24) | ((uint32_t)ip->src[1] << 16) |
                       ((uint32_t)ip->src[2] << 8) | ip->src[3];
        ip_send(IP_PROTO_ICMP, src, reply, (uint16_t)reply_len);
    }
}

void icmp_ping(uint32_t ip) {
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    struct icmp_hdr *ic = (struct icmp_hdr *)pkt;
    ic->type = ICMP_ECHO_REQUEST;
    ic->code = 0;
    ic->id[0] = 0xAB; ic->id[1] = 0xCD;
    ic->seq[0] = 0x00; ic->seq[1] = 0x01;
    uint16_t csum_val = net_csum(pkt, sizeof(pkt));
    ic->csum[0] = (csum_val >> 8) & 0xFF;
    ic->csum[1] = csum_val & 0xFF;
    ip_send(IP_PROTO_ICMP, ip, pkt, sizeof(pkt));
}
