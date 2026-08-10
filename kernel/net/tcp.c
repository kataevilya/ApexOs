#include "net.h"
#include "rtl8139.h"
#include "serial.h"
#include "string.h"

void ip_send(uint8_t proto, uint32_t dst, const void *payload, uint16_t plen);
#define TCP_RX_BUF_SIZE 4096

static uint8_t tcp_rx_buf[TCP_RX_BUF_SIZE];
static uint32_t tcp_rx_len = 0;
static int tcp_connected = 0;
static uint32_t tcp_remote_ip = 0;
static uint16_t tcp_remote_port = 0;
static uint32_t tcp_our_port = 49152;
static uint32_t tcp_seq = 12345;
static uint32_t tcp_ack = 0;

static uint16_t tcp_checksum(uint32_t src, uint32_t dst, const void *buf, size_t len) {
    struct ip_pseudo_hdr ph;
    for (int i = 0; i < 4; i++) {
        ph.src[i] = (src >> (24 - i * 8)) & 0xFF;
        ph.dst[i] = (dst >> (24 - i * 8)) & 0xFF;
    }
    ph.zero = 0;
    ph.proto = IP_PROTO_TCP;
    ph.len[0] = (len >> 8) & 0xFF;
    ph.len[1] = len & 0xFF;
    uint16_t c = net_csum(&ph, sizeof(ph));
    c = net_csum_combine(c, buf, len);
    return c;
}

int net_tcp_connect(uint32_t ip, uint16_t port) {
    tcp_remote_ip = ip;
    tcp_remote_port = port;
    tcp_our_port = 49152 + (tcp_our_port % 10000);
    tcp_seq = 12345;
    tcp_ack = 0;
    tcp_rx_len = 0;
    tcp_connected = 0;

    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    struct tcp_hdr *th = (struct tcp_hdr *)pkt;
    th->sport[0] = (tcp_our_port >> 8) & 0xFF;
    th->sport[1] = tcp_our_port & 0xFF;
    th->dport[0] = (port >> 8) & 0xFF;
    th->dport[1] = port & 0xFF;
    th->seq[0] = (tcp_seq >> 24) & 0xFF;
    th->seq[1] = (tcp_seq >> 16) & 0xFF;
    th->seq[2] = (tcp_seq >> 8) & 0xFF;
    th->seq[3] = tcp_seq & 0xFF;
    th->off = 0x50;
    th->flags = TCP_SYN;
    th->win[0] = 0xFF; th->win[1] = 0xFF;
    uint16_t c = tcp_checksum(g_net.ip_addr, ip, pkt, sizeof(struct tcp_hdr));
    th->csum[0] = (c >> 8) & 0xFF;
    th->csum[1] = c & 0xFF;

    ip_send(IP_PROTO_TCP, ip, pkt, sizeof(struct tcp_hdr));

    for (volatile int i = 0; i < 500000; i++) __asm__ volatile ("nop");
    if (!tcp_connected) {
        return -1;
    }
    return 0;
}

int net_tcp_send(const void *buf, size_t len) {
    if (!tcp_connected) return -1;
    uint8_t pkt[2048];
    size_t pkt_len = sizeof(struct tcp_hdr) + len;
    if (pkt_len > sizeof(pkt)) return -1;

    struct tcp_hdr *th = (struct tcp_hdr *)pkt;
    th->sport[0] = (tcp_our_port >> 8) & 0xFF;
    th->sport[1] = tcp_our_port & 0xFF;
    th->dport[0] = (tcp_remote_port >> 8) & 0xFF;
    th->dport[1] = tcp_remote_port & 0xFF;
    th->seq[0] = (tcp_seq >> 24) & 0xFF;
    th->seq[1] = (tcp_seq >> 16) & 0xFF;
    th->seq[2] = (tcp_seq >> 8) & 0xFF;
    th->seq[3] = tcp_seq & 0xFF;
    th->ack[0] = (tcp_ack >> 24) & 0xFF;
    th->ack[1] = (tcp_ack >> 16) & 0xFF;
    th->ack[2] = (tcp_ack >> 8) & 0xFF;
    th->ack[3] = tcp_ack & 0xFF;
    th->off = 0x50;
    th->flags = TCP_PSH | TCP_ACK;
    th->win[0] = 0xFF; th->win[1] = 0xFF;
    memcpy(pkt + sizeof(struct tcp_hdr), buf, len);
    uint16_t c = tcp_checksum(g_net.ip_addr, tcp_remote_ip, pkt, pkt_len);
    th->csum[0] = (c >> 8) & 0xFF;
    th->csum[1] = c & 0xFF;

    ip_send(IP_PROTO_TCP, tcp_remote_ip, pkt, pkt_len);
    tcp_seq += len;
    return 0;
}

int net_tcp_recv(void *buf, size_t max_len) {
    if (tcp_rx_len == 0) return 0;
    size_t copy = tcp_rx_len;
    if (copy > max_len) copy = max_len;
    memcpy(buf, tcp_rx_buf, copy);
    tcp_rx_len = 0;
    return (int)copy;
}

void net_tcp_close(void) {
    if (!tcp_connected) return;
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    struct tcp_hdr *th = (struct tcp_hdr *)pkt;
    th->sport[0] = (tcp_our_port >> 8) & 0xFF;
    th->sport[1] = tcp_our_port & 0xFF;
    th->dport[0] = (tcp_remote_port >> 8) & 0xFF;
    th->dport[1] = tcp_remote_port & 0xFF;
    th->seq[0] = (tcp_seq >> 24) & 0xFF;
    th->seq[1] = (tcp_seq >> 16) & 0xFF;
    th->seq[2] = (tcp_seq >> 8) & 0xFF;
    th->seq[3] = tcp_seq & 0xFF;
    th->ack[0] = (tcp_ack >> 24) & 0xFF;
    th->ack[1] = (tcp_ack >> 16) & 0xFF;
    th->ack[2] = (tcp_ack >> 8) & 0xFF;
    th->ack[3] = tcp_ack & 0xFF;
    th->off = 0x50;
    th->flags = TCP_FIN | TCP_ACK;
    th->win[0] = 0xFF; th->win[1] = 0xFF;
    uint16_t c = tcp_checksum(g_net.ip_addr, tcp_remote_ip, pkt, sizeof(struct tcp_hdr));
    th->csum[0] = (c >> 8) & 0xFF;
    th->csum[1] = c & 0xFF;
    ip_send(IP_PROTO_TCP, tcp_remote_ip, pkt, sizeof(struct tcp_hdr));
    tcp_connected = 0;
}

int net_tcp_is_connected(void) {
    return tcp_connected;
}

void tcp_handle(const void *buf, size_t len) {
    if (len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)buf;
    uint16_t sport = ((uint16_t)th->sport[0] << 8) | th->sport[1];
    uint16_t dport = ((uint16_t)th->dport[0] << 8) | th->dport[1];
    uint8_t flags = th->flags;

    if (sport != tcp_remote_port || dport != tcp_our_port) return;

    uint32_t seq = ((uint32_t)th->seq[0] << 24) | ((uint32_t)th->seq[1] << 16) |
                   ((uint32_t)th->seq[2] << 8) | th->seq[3];
    uint32_t ack = ((uint32_t)th->ack[0] << 24) | ((uint32_t)th->ack[1] << 16) |
                   ((uint32_t)th->ack[2] << 8) | th->ack[3];

    if (flags & TCP_SYN && !tcp_connected) {
        tcp_ack = seq + 1;
        tcp_seq += 1;
        tcp_connected = 1;
        serial_write("[tcp] connection established\n");
        uint8_t pkt[64];
        memset(pkt, 0, sizeof(pkt));
        struct tcp_hdr *r = (struct tcp_hdr *)pkt;
        r->sport[0] = (tcp_our_port >> 8) & 0xFF;
        r->sport[1] = tcp_our_port & 0xFF;
        r->dport[0] = (tcp_remote_port >> 8) & 0xFF;
        r->dport[1] = tcp_remote_port & 0xFF;
        r->seq[0] = (tcp_seq >> 24) & 0xFF;
        r->seq[1] = (tcp_seq >> 16) & 0xFF;
        r->seq[2] = (tcp_seq >> 8) & 0xFF;
        r->seq[3] = tcp_seq & 0xFF;
        r->ack[0] = (tcp_ack >> 24) & 0xFF;
        r->ack[1] = (tcp_ack >> 16) & 0xFF;
        r->ack[2] = (tcp_ack >> 8) & 0xFF;
        r->ack[3] = tcp_ack & 0xFF;
        r->off = 0x50;
        r->flags = TCP_ACK;
        r->win[0] = 0xFF; r->win[1] = 0xFF;
        uint16_t c = tcp_checksum(g_net.ip_addr, tcp_remote_ip, pkt, sizeof(struct tcp_hdr));
        r->csum[0] = (c >> 8) & 0xFF;
        r->csum[1] = c & 0xFF;
        ip_send(IP_PROTO_TCP, tcp_remote_ip, pkt, sizeof(struct tcp_hdr));
    } else if (flags & TCP_ACK) {
        if (ack > tcp_seq) tcp_seq = ack;
        uint8_t hdr_len = (th->off >> 4) * 4;
        if (hdr_len < sizeof(struct tcp_hdr)) hdr_len = sizeof(struct tcp_hdr);
        const uint8_t *payload = (const uint8_t *)buf + hdr_len;
        size_t plen = len - hdr_len;
        if (plen > 0 && tcp_rx_len + plen <= TCP_RX_BUF_SIZE) {
            memcpy(tcp_rx_buf + tcp_rx_len, payload, plen);
            tcp_rx_len += plen;
            tcp_ack = seq + plen;
            uint8_t pkt[64];
            memset(pkt, 0, sizeof(pkt));
            struct tcp_hdr *r = (struct tcp_hdr *)pkt;
            r->sport[0] = (tcp_our_port >> 8) & 0xFF;
            r->sport[1] = tcp_our_port & 0xFF;
            r->dport[0] = (tcp_remote_port >> 8) & 0xFF;
            r->dport[1] = tcp_remote_port & 0xFF;
            r->seq[0] = (tcp_seq >> 24) & 0xFF;
            r->seq[1] = (tcp_seq >> 16) & 0xFF;
            r->seq[2] = (tcp_seq >> 8) & 0xFF;
            r->seq[3] = tcp_seq & 0xFF;
            r->ack[0] = (tcp_ack >> 24) & 0xFF;
            r->ack[1] = (tcp_ack >> 16) & 0xFF;
            r->ack[2] = (tcp_ack >> 8) & 0xFF;
            r->ack[3] = tcp_ack & 0xFF;
            r->off = 0x50;
            r->flags = TCP_ACK;
            r->win[0] = 0xFF; r->win[1] = 0xFF;
            uint16_t c = tcp_checksum(g_net.ip_addr, tcp_remote_ip, pkt, sizeof(struct tcp_hdr));
            r->csum[0] = (c >> 8) & 0xFF;
            r->csum[1] = c & 0xFF;
            ip_send(IP_PROTO_TCP, tcp_remote_ip, pkt, sizeof(struct tcp_hdr));
        }
    }
    if (flags & TCP_FIN) {
        tcp_connected = 0;
    }
}
