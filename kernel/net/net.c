#include "net.h"
#include "rtl8139.h"
#include "serial.h"
#include "console.h"
#include "string.h"
#include <stdint.h>
void arp_handle(const void *buf, size_t len);
void ip_handle(const void *buf, size_t len);

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
    } else if (type == ETH_TYPE_IP && plen >= sizeof(struct ip_hdr)) {
        ip_handle(payload, plen);
    }
}

void net_tx_poll(void) {
}
