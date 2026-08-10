#include "net.h"
#include "rtl8139.h"
#include "serial.h"
#include "string.h"
#include <stdint.h>

#define DHCP_SERVER_PORT  67
#define DHCP_CLIENT_PORT  68

void dhcp_set_callback(dhcp_callback_t cb) {
    g_dhcp_cb = cb;
}

static void dhcp_handle(uint16_t sport, const void *buf, size_t len) {
    if (sport != DHCP_SERVER_PORT) return;
    if (len < 240 || g_dhcp_cb == NULL) return;
    const uint8_t *p = (const uint8_t *)buf;
    if (p[0] != 0x02) return;

    uint32_t offered_ip = ((uint32_t)p[16] << 24) | ((uint32_t)p[17] << 16) |
                          ((uint32_t)p[18] << 8) | p[19];
    uint32_t server_ip = ((uint32_t)p[20] << 24) | ((uint32_t)p[21] << 16) |
                         ((uint32_t)p[22] << 8) | p[23];

    g_dhcp_cb(offered_ip, server_ip);
}

void dhcp_discover(void) {
    uint8_t pkt[300];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x01;
    pkt[1] = 0x01;
    pkt[2] = 0x06;
    pkt[3] = 0x00;
    pkt[4] = 0xAB;
    pkt[5] = 0xCD;
    pkt[6] = 0x00;
    pkt[7] = 0x00;
    pkt[8] = 0x00;
    pkt[9] = 0x00;
    pkt[10] = 0x00;
    pkt[11] = 0x00;
    pkt[28] = 0x00;
    pkt[29] = 0x00;
    pkt[30] = 0x00;
    pkt[31] = 0x00;
    pkt[236] = 0x63;
    pkt[237] = 0x82;
    pkt[238] = 0x53;
    pkt[239] = 0x63;
    pkt[240] = 0x35;
    pkt[241] = 0x01;
    pkt[242] = 0x01;
    pkt[243] = 0x3D;
    pkt[244] = 0x07;
    pkt[245] = 0x01;
    pkt[246] = 'a';
    pkt[247] = 'p';
    pkt[248] = 'e';
    pkt[249] = 'x';
    pkt[250] = 'o';
    pkt[251] = 's';
    pkt[252] = 0xFF;

    udp_set_callback(dhcp_handle, DHCP_CLIENT_PORT);
    uint32_t broadcast_ip = 0xFFFFFFFF;
    udp_send(DHCP_SERVER_PORT, broadcast_ip, pkt, 253);
}
