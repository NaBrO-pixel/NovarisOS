#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>

/* Finds and brings up an RTL8139 if the machine has one. Returns 1 if a
 * card is now sending and receiving, 0 if there is none - which is not an
 * error, it is a machine with no network card. */
int rtl8139_init(void);

int rtl8139_present(void);
uint16_t rtl8139_io_base(void);
uint8_t rtl8139_irq(void);

/* Drains the receive ring now rather than waiting for the next
 * interrupt. See the note where it is defined. */
void rtl8139_poll(void);

/* Diagnostics: how many interrupts the card raised, how many frames were
 * drained from inside one, how many by polling instead, and how often a
 * send found every transmit buffer busy. */
uint32_t rtl8139_irq_count(void);
uint32_t rtl8139_irq_frames(void);
uint32_t rtl8139_poll_frames(void);
uint32_t rtl8139_tx_busy(void);

#endif /* RTL8139_H */
