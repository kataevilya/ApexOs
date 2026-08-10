#include "net.h"
#include "rtl8139.h"
#include "serial.h"
#include "string.h"

void eth_send(const uint8_t *dst_mac, uint16_t ethertype, const void *payload, size_t len) {
    uint8_t frame[2048];
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    for (int i = 0; i < ETH_ALEN; i++) {
        eh->dst[i] = dst_mac[i];
        eh->src[i] = g_net.mac[i];
    }
    eh->type[0] = (ethertype >> 8) & 0xFF;
    eh->type[1] = ethertype & 0xFF;
    memcpy(frame + ETH_HLEN, payload, len);
    net_send_packet(frame, ETH_HLEN + len);
}
