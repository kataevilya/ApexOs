#include "net.h"
#include "rtl8139.h"
#include "serial.h"
#include "string.h"

static uint16_t ip_id = 0;

void icmp_handle(const void *buf, size_t len);
void tcp_handle(const void *buf, size_t len);

void ip_handle(const void *buf, size_t len) {
    if (len < sizeof(struct ip_hdr)) return;
    const struct ip_hdr *ip = (const struct ip_hdr *)buf;
    uint8_t ihl = ip->ver_ihl & 0x0F;
    if (ihl < 5) return;
    uint16_t total = ((uint16_t)ip->len[0] << 8) | ip->len[1];
    if (total < (ihl * 4) || total > len) return;

    uint32_t dst = ((uint32_t)ip->dst[0] << 24) | ((uint32_t)ip->dst[1] << 16) |
                   ((uint32_t)ip->dst[2] << 8) | ip->dst[3];
    if (dst != g_net.ip_addr && dst != 0xFFFFFFFF) return;

    const uint8_t *payload = (const uint8_t *)buf + ihl * 4;
    size_t plen = total - ihl * 4;
    uint8_t proto = ip->proto;

    if (proto == IP_PROTO_ICMP && plen >= sizeof(struct icmp_hdr)) {
        icmp_handle(payload, plen);
    } else if (proto == IP_PROTO_UDP && plen >= sizeof(struct udp_hdr)) {
        udp_handle(payload, plen);
    } else if (proto == IP_PROTO_TCP && plen >= sizeof(struct tcp_hdr)) {
        tcp_handle(payload, plen);
    }
}

uint16_t net_csum(const void *buf, size_t len) {
    const uint16_t *p = (const uint16_t *)buf;
    uint32_t sum = 0;
    for (size_t i = 0; i < len / 2; i++) sum += p[i];
    if (len & 1) sum += ((const uint8_t *)buf)[len - 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

uint16_t net_csum_combine(uint16_t a, const void *buf, size_t len) {
    const uint16_t *p = (const uint16_t *)buf;
    uint32_t sum = a;
    for (size_t i = 0; i < len / 2; i++) sum += p[i];
    if (len & 1) sum += ((const uint8_t *)buf)[len - 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}

static void build_ip_hdr(struct ip_hdr *ip, uint8_t proto, uint32_t dst, const void *payload, uint16_t plen) {
    (void)payload;
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len[0] = (sizeof(struct ip_hdr) + plen) >> 8;
    ip->len[1] = (sizeof(struct ip_hdr) + plen) & 0xFF;
    ip->id[0] = (ip_id >> 8) & 0xFF;
    ip->id[1] = ip_id & 0xFF;
    ip_id++;
    ip->frag_off[0] = 0x40;
    ip->frag_off[1] = 0x00;
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum[0] = 0; ip->csum[1] = 0;
    ip->src[0] = (g_net.ip_addr >> 24) & 0xFF;
    ip->src[1] = (g_net.ip_addr >> 16) & 0xFF;
    ip->src[2] = (g_net.ip_addr >> 8) & 0xFF;
    ip->src[3] = g_net.ip_addr & 0xFF;
    ip->dst[0] = (dst >> 24) & 0xFF;
    ip->dst[1] = (dst >> 16) & 0xFF;
    ip->dst[2] = (dst >> 8) & 0xFF;
    ip->dst[3] = dst & 0xFF;
    uint16_t c = net_csum(ip, sizeof(struct ip_hdr));
    ip->csum[0] = (c >> 8) & 0xFF;
    ip->csum[1] = c & 0xFF;
}

void ip_send(uint8_t proto, uint32_t dst, const void *payload, uint16_t plen) {
    uint8_t frame[2048];
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    struct ip_hdr *ip = (struct ip_hdr *)(frame + ETH_HLEN);

    if (dst == 0xFFFFFFFF || (dst & g_net.netmask) == (g_net.ip_addr & g_net.netmask)) {
        dst = g_net.gateway;
    }

    for (int i = 0; i < ETH_ALEN; i++) eh->dst[i] = 0xFF;
    for (int i = 0; i < ETH_ALEN; i++) eh->src[i] = g_net.mac[i];
    eh->type[0] = (ETH_TYPE_IP >> 8) & 0xFF;
    eh->type[1] = ETH_TYPE_IP & 0xFF;

    memcpy(frame + ETH_HLEN + sizeof(struct ip_hdr), payload, plen);
    build_ip_hdr(ip, proto, dst, payload, plen);
    net_send_packet(frame, ETH_HLEN + sizeof(struct ip_hdr) + plen);
}
