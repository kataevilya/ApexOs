#ifndef APEXOS_NET_H
#define APEXOS_NET_H

#include <stdint.h>
#include <stddef.h>

#define ETH_ALEN   6
#define ETH_HLEN   14
#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint8_t  type[2];
} __attribute__((packed));

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint8_t  len[2];
    uint8_t  id[2];
    uint8_t  frag_off[2];
    uint8_t  ttl;
    uint8_t  proto;
    uint8_t  csum[2];
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed));

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2
#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP  ETH_TYPE_IP

struct arp_hdr {
    uint8_t  htype[2];
    uint8_t  ptype[2];
    uint8_t  hlen;
    uint8_t  plen;
    uint8_t  oper[2];
    uint8_t  sha[ETH_ALEN];
    uint8_t  spa[4];
    uint8_t  tha[ETH_ALEN];
    uint8_t  tpa[4];
} __attribute__((packed));

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint8_t  csum[2];
    uint8_t  id[2];
    uint8_t  seq[2];
} __attribute__((packed));

struct udp_hdr {
    uint8_t  sport[2];
    uint8_t  dport[2];
    uint8_t  len[2];
    uint8_t  csum[2];
} __attribute__((packed));

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

struct tcp_hdr {
    uint8_t  sport[2];
    uint8_t  dport[2];
    uint8_t  seq[4];
    uint8_t  ack[4];
    uint8_t  off;
    uint8_t  flags;
    uint8_t  win[2];
    uint8_t  csum[2];
    uint8_t  urg[2];
} __attribute__((packed));

struct ip_pseudo_hdr {
    uint8_t  src[4];
    uint8_t  dst[4];
    uint8_t  zero;
    uint8_t  proto;
    uint8_t  len[2];
} __attribute__((packed));

typedef struct {
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    uint8_t  mac[ETH_ALEN];
    uint32_t dns_server;
    int      have_ip;
} net_state_t;

typedef void (*dhcp_callback_t)(uint32_t offered_ip, uint32_t server_ip);
typedef void (*dns_callback_t)(uint32_t ip);
typedef void (*http_callback_t)(const void *data, size_t len);
typedef void (*udp_callback_t)(uint16_t sport, const void *data, size_t len);

extern net_state_t g_net;
extern dhcp_callback_t g_dhcp_cb;

void    net_init(void);
void    net_send_packet(const void *buf, size_t len);
void    net_rx_handler(const void *buf, size_t len);
void    net_tx_poll(void);

uint16_t net_csum(const void *buf, size_t len);
uint16_t net_csum_combine(uint16_t a, const void *buf, size_t len);

void    dhcp_set_callback(dhcp_callback_t cb);
void    dhcp_discover(void);

int     net_dns_resolve(const char *hostname, uint32_t *out_ip);
void    dns_set_callback(dns_callback_t cb);

void    udp_set_callback(udp_callback_t cb, uint16_t port);
void    udp_handle(const void *buf, size_t len);
void    udp_send(uint16_t dst_port, uint32_t ip, const void *payload, uint16_t plen);

int     net_tcp_connect(uint32_t ip, uint16_t port);
int     net_tcp_send(const void *buf, size_t len);
int     net_tcp_recv(void *buf, size_t max_len);
void    net_tcp_close(void);
int     net_tcp_is_connected(void);

int     net_http_get(const char *host, const char *path, char *buf, size_t max_len, uint32_t *out_len);

#endif /* APEXOS_NET_H */
