#include "net.h"
#include "rtl8139.h"
#include "serial.h"
#include "console.h"
#include "string.h"
#include "pit.h"
#include <stdint.h>
void arp_handle(const void *buf, size_t len);
void ip_handle(const void *buf, size_t len);

#define ARP_CACHE_SIZE 16

static struct {
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    int valid;
} g_arp_cache[ARP_CACHE_SIZE];

static uint32_t g_arp_pending_ip = 0;
static int g_arp_done = 0;
static uint8_t g_arp_resolved_mac[ETH_ALEN];

static void arp_cache_add(uint32_t ip, const uint8_t *mac);
static void arp_wakeup(uint32_t ip, const uint8_t *mac);

net_state_t g_net;
dhcp_callback_t g_dhcp_cb = NULL;

static void dhcp_cb(uint32_t offered_ip, uint32_t server_ip) {
    g_net.ip_addr = offered_ip;
    g_net.netmask = 0xFFFFFF00;
    g_net.gateway = server_ip;
    g_net.dns_server = server_ip;
    g_net.have_ip = 1;
    console_printf("[net] DHCP offered: %d.%d.%d.%d, gateway=%d.%d.%d.%d\n",
                   (offered_ip >> 24) & 0xFF, (offered_ip >> 16) & 0xFF,
                   (offered_ip >> 8) & 0xFF, offered_ip & 0xFF,
                   (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
                   (server_ip >> 8) & 0xFF, server_ip & 0xFF);
    serial_printf("[net] DHCP offered: %d.%d.%d.%d, gateway=%d.%d.%d.%d\n",
                  (offered_ip >> 24) & 0xFF, (offered_ip >> 16) & 0xFF,
                  (offered_ip >> 8) & 0xFF, offered_ip & 0xFF,
                  (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
                  (server_ip >> 8) & 0xFF, server_ip & 0xFF);
}

void net_init(void) {
    memset(&g_net, 0, sizeof(g_net));
    rtl8139_init();
    rtl8139_get_mac(g_net.mac);
    console_printf("[net] initialized, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   g_net.mac[0], g_net.mac[1], g_net.mac[2],
                   g_net.mac[3], g_net.mac[4], g_net.mac[5]);
    serial_printf("[net] initialized, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  g_net.mac[0], g_net.mac[1], g_net.mac[2],
                  g_net.mac[3], g_net.mac[4], g_net.mac[5]);

    dhcp_set_callback(dhcp_cb);
    dhcp_discover();

    for (volatile int i = 0; i < 500000; i++) __asm__ volatile ("nop");
    for (int poll = 0; poll < 200; poll++) {
        net_tx_poll();
        for (volatile int j = 0; j < 50000; j++) __asm__ volatile ("nop");
    }

    if (!g_net.have_ip) {
        console_write("[net] DHCP timeout, using fallback 10.0.2.15/24\n");
        serial_write("[net] DHCP timeout, using fallback 10.0.2.15/24\n");
        g_net.ip_addr = 0x0A00020F;
        g_net.netmask = 0xFFFFFF00;
        g_net.gateway = 0x0A000202;
        g_net.dns_server = 0x0A000202;
        g_net.have_ip = 1;
    }
}

void net_send_packet(const void *buf, size_t len) {
    rtl8139_send(buf, len);
}

void net_rx_handler(const void *buf, size_t len) {
    if (len < ETH_HLEN) return;
    const struct eth_hdr *eth = (const struct eth_hdr *)buf;
    uint16_t type = ((uint16_t)eth->type[0] << 8) | eth->type[1];
    const uint8_t *payload = (const uint8_t *)buf + ETH_HLEN;
    size_t plen = len - ETH_HLEN;

    if (type == ETH_TYPE_ARP && plen >= sizeof(struct arp_hdr)) {
        arp_handle(payload, plen);
        const struct arp_hdr *arp = (const struct arp_hdr *)payload;
        uint32_t spa = ((uint32_t)arp->spa[0] << 24) | ((uint32_t)arp->spa[1] << 16) |
                       ((uint32_t)arp->spa[2] << 8) | arp->spa[3];
        if (spa != 0 && spa != g_net.ip_addr) {
            arp_cache_add(spa, arp->sha);
            arp_wakeup(spa, arp->sha);
        }
    } else if (type == ETH_TYPE_IP && plen >= sizeof(struct ip_hdr)) {
        ip_handle(payload, plen);
    }
}

void net_tx_poll(void) {
    uint8_t buf[2048];
    size_t len;
    while (rtl8139_poll(buf, sizeof(buf), &len)) {
        net_rx_handler(buf, len);
    }
}

static void arp_cache_add(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            for (int j = 0; j < ETH_ALEN; j++) g_arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_arp_cache[i].valid) {
            g_arp_cache[i].ip = ip;
            for (int j = 0; j < ETH_ALEN; j++) g_arp_cache[i].mac[j] = mac[j];
            g_arp_cache[i].valid = 1;
            return;
        }
    }
}

static int arp_cache_lookup(uint32_t ip, uint8_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            for (int j = 0; j < ETH_ALEN; j++) out_mac[j] = g_arp_cache[i].mac[j];
            return 0;
        }
    }
    return -1;
}

static void arp_wakeup(uint32_t ip, const uint8_t *mac) {
    if (g_arp_pending_ip == ip && !g_arp_done) {
        for (int j = 0; j < ETH_ALEN; j++) g_arp_resolved_mac[j] = mac[j];
        g_arp_done = 1;
    }
}

int arp_resolve(uint32_t ip, uint8_t *out_mac) {
    if (ip == 0xFFFFFFFF) {
        for (int j = 0; j < ETH_ALEN; j++) out_mac[j] = 0xFF;
        return 0;
    }

    if (arp_cache_lookup(ip, out_mac) == 0) {
        return 0;
    }

    for (int retry = 0; retry < 5; retry++) {
        g_arp_pending_ip = ip;
        g_arp_done = 0;
        arp_send_request(ip);

        uint64_t deadline = pit_get_ticks() + 200;
        while (!g_arp_done && pit_get_ticks() < deadline) {
            net_tx_poll();
            for (volatile int i = 0; i < 50000; i++) __asm__ volatile ("nop");
        }

        if (g_arp_done) {
            for (int j = 0; j < ETH_ALEN; j++) out_mac[j] = g_arp_resolved_mac[j];
            arp_cache_add(ip, out_mac);
            return 0;
        }

        console_printf("[arp] retry %d for %u.%u.%u.%u\n",
                       retry + 1,
                       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                       (ip >> 8) & 0xFF, ip & 0xFF);
        serial_printf("[arp] retry %d for %u.%u.%u.%u\n",
                      retry + 1,
                      (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                      (ip >> 8) & 0xFF, ip & 0xFF);
    }

    console_printf("[arp] timeout resolving %u.%u.%u.%u\n",
                   (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                   (ip >> 8) & 0xFF, ip & 0xFF);
    serial_printf("[arp] timeout resolving %u.%u.%u.%u\n",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                  (ip >> 8) & 0xFF, ip & 0xFF);
    return -1;
}
