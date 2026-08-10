#ifndef APEXOS_RTL8139_H
#define APEXOS_RTL8139_H

#include <stdint.h>
#include <stddef.h>

#define RTL8139_MAC0    0x00
#define RTL8139_MAR0    0x08
#define RTL8139_TXSTAT0 0x10
#define RTL8139_TXADDR0 0x20
#define RTL8139_RXBUF   0x30
#define RTL8139_CMD     0x37
#define RTL8139_CAPR    0x38
#define RTL8139_IMR     0x3C
#define RTL8139_ISR     0x3E
#define RTL8139_TCR     0x40
#define RTL8139_RCR     0x44
#define RTL8139_CONFIG1 0x52

#define RTL_CMD_RESET   0x10
#define RTL_CMD_RX_ENA  0x08
#define RTL_CMD_TX_ENA  0x04

#define RTL_ISR_ROK     0x01
#define RTL_ISR_RER     0x02
#define RTL_ISR_TOK     0x04
#define RTL_ISR_TER     0x08

#define RTL_RCR_RX_ALL  0x0F
#define RTL_RCR_RX_AP   0x02

#define RTL_RXBUF_SIZE  (64 * 1024)

void rtl8139_init(void);
void rtl8139_send(const void *data, size_t len);
int  rtl8139_poll(void *buf, size_t max_len, size_t *out_len);
void rtl8139_get_mac(uint8_t mac[6]);

#endif /* APEXOS_RTL8139_H */
