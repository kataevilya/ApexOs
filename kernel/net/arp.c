#include "net.h"
#include "serial.h"
#include "console.h"
#include "string.h"

void arp_handle(const void *buf, size_t len) {
    if (len < sizeof(struct arp_hdr)) return;
    const struct arp_hdr *arp = (const struct arp_hdr *)buf;
    uint16_t htype = ((uint16_t)arp->htype[0] << 8) | arp->htype[1];
    uint16_t ptype = ((uint16_t)arp->ptype[0] << 8) | arp->ptype[1];
    if (htype != ARP_HTYPE_ETH || ptype != ARP_PTYPE_IP) return;

    uint16_t oper = ((uint16_t)arp->oper[0] << 8) | arp->oper[1];
    uint32_t spa = ((uint32_t)arp->spa[0] << 24) | ((uint32_t)arp->spa[1] << 16) |
                   ((uint32_t)arp->spa[2] << 8) | arp->spa[3];
    uint32_t tpa = ((uint32_t)arp->tpa[0] << 24) | ((uint32_t)arp->tpa[1] << 16) |
                   ((uint32_t)arp->tpa[2] << 8) | arp->tpa[3];

    if (oper == ARP_OP_REQUEST && tpa == g_net.ip_addr) {
        console_printf("[arp] reply to %u.%u.%u.%u\n",
                       (spa >> 24) & 0xFF, (spa >> 16) & 0xFF,
                       (spa >> 8) & 0xFF, spa & 0xFF);
        serial_printf("[arp] reply to %u.%u.%u.%u\n",
                      (spa >> 24) & 0xFF, (spa >> 16) & 0xFF,
                      (spa >> 8) & 0xFF, spa & 0xFF);
        uint8_t reply[sizeof(struct arp_hdr)];
        struct arp_hdr *r = (struct arp_hdr *)reply;
        r->htype[0] = (ARP_HTYPE_ETH >> 8) & 0xFF;
        r->htype[1] = ARP_HTYPE_ETH & 0xFF;
        r->ptype[0] = (ARP_PTYPE_IP >> 8) & 0xFF;
        r->ptype[1] = ARP_PTYPE_IP & 0xFF;
        r->hlen = ETH_ALEN;
        r->plen = 4;
        r->oper[0] = (ARP_OP_REPLY >> 8) & 0xFF;
        r->oper[1] = ARP_OP_REPLY & 0xFF;
        for (int i = 0; i < ETH_ALEN; i++) {
            r->sha[i] = g_net.mac[i];
            r->tha[i] = arp->sha[i];
        }
        for (int i = 0; i < 4; i++) {
            r->spa[i] = (g_net.ip_addr >> (24 - i * 8)) & 0xFF;
            r->tpa[i] = (spa >> (24 - i * 8)) & 0xFF;
        }

        uint8_t frame[2048];
        struct eth_hdr *eh = (struct eth_hdr *)frame;
        for (int i = 0; i < ETH_ALEN; i++) {
            eh->dst[i] = arp->sha[i];
            eh->src[i] = g_net.mac[i];
        }
        eh->type[0] = (ETH_TYPE_ARP >> 8) & 0xFF;
        eh->type[1] = ETH_TYPE_ARP & 0xFF;
        memcpy(frame + ETH_HLEN, reply, sizeof(reply));
        net_send_packet(frame, ETH_HLEN + sizeof(reply));
    }
}
