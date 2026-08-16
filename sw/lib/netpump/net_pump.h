/*
 * net_pump.h — the one place this project services the lwIP receive path.
 *
 * Contract: doc/icd_control_channel.md § One pump, one owner.
 *
 * There is no OS, no RX thread and — because the BSP builds with NO_SYS=1 and
 * NO_SYS_NO_TIMERS=1 — no lwIP timeout subsystem either. Two things therefore
 * have to be driven by hand from whatever loop the application happens to be
 * in: delivering received frames, and the ARP timer. Both are here.
 *
 * Every wait loop in this project goes through net_pump(). That is a
 * requirement, not a convention: the ARP timer has exactly one owner and
 * TcpFastTmrFlag has exactly one consumer, and a second copy of this function
 * would quietly break both.
 */

#ifndef NET_PUMP_H
#define NET_PUMP_H

#include "lwip/netif.h"
#include "xil_types.h"

/*
 * Give the stack one chance to deliver packets and drive the ARP timer.
 *
 * Returns 1 if a 250 ms platform tick elapsed during this call, 0 otherwise —
 * so a caller can build a timeout out of the same call it uses to receive,
 * without a second time source. Callers accumulate the return value; four ticks
 * is one second.
 *
 * Cheap enough to call in a tight loop and safe to call before any transfer is
 * in progress; it does nothing but service whatever the MAC has.
 */
u32_t net_pump(struct netif *netif);

/*
 * Pump for approximately `ms` milliseconds and discard the tick count.
 *
 * For the one case that is a genuine delay rather than a wait for something:
 * letting a reply drain out of the MAC before deliberately resetting the board.
 * Rounded up to the 250 ms tick — this cannot resolve finer and does not
 * pretend to.
 */
void net_pump_ms(struct netif *netif, u32_t ms);

#endif /* NET_PUMP_H */
