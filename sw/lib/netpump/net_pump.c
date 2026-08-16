/*
 * net_pump.c — service the lwIP receive path and the ARP timer, by hand.
 *
 * Moved verbatim out of tftp_client.c when the control channel became a second
 * caller. Nothing about the logic changed; what changed is that there is now
 * one copy of it instead of the two that were about to exist.
 *
 * WHY etharp_tmr() IS CALLED HERE — this was the bug that cost a whole session.
 *
 * The BSP builds with NO_SYS=1 and NO_SYS_NO_TIMERS=1, so LWIP_TIMERS evaluates
 * to 0 and lwIP's entire timeout subsystem is compiled out. The cyclic table in
 * timeouts.c that carries {ARP_TMR_INTERVAL, etharp_tmr} never runs, there is no
 * sys_check_timeouts() to call, and nothing in the Xilinx platform layer calls
 * etharp_tmr() either — platform_zynq.c's ISR drives only tcp_fasttmr,
 * tcp_slowtmr and the DHCP timers.
 *
 * That is fatal for anything that INITIATES traffic, because of how
 * etharp_query() works (etharp.c): it sends an ARP request only when it creates
 * a new entry — `if (is_new_entry || (q == NULL))`. Every later call for an
 * address already in ETHARP_STATE_PENDING just queues the packet behind it and
 * sends nothing. Retransmission of the request is etharp_tmr()'s job alone; the
 * source even says so at the point the entry is created: "record network
 * interface for re-sending arp request in etharp_tmr".
 *
 * So with no ARP timer, exactly ONE ARP request is ever sent per address. Lose
 * it and the entry stays PENDING forever: udp_sendto() keeps returning ERR_OK,
 * not one datagram is ever transmitted, and the application sees only a
 * timeout. Retrying at the application layer cannot help — every retry lands on
 * the same pending entry.
 *
 * At boot that first request is very likely to be lost, because the PHY is
 * still negotiating when it goes out. The TFTP client was the first app in the
 * project that transmits at boot — the register app only prints and the echo
 * server is passive, waiting to be contacted, so neither ever had to resolve an
 * address and neither exposed it.
 *
 * The tell, once the client reported ARP state: `server MAC UNRESOLVED` for all
 * 30 retries, over 30 s, on a direct point-to-point cable with a host whose
 * arp_ignore and arp_filter are both 0. And a single ping FROM the host fixed it
 * instantly — not because the host taught us anything special, but because
 * etharp_input() updates a pending entry from any ARP packet it sees, which
 * flushes the queued datagram.
 *
 * WHY ONE COPY MATTERS
 *
 * arp_ticks below is static and TcpFastTmrFlag is consumed here — the flag is
 * cleared as it is read, so whichever pump runs first swallows the tick. Two
 * pumps in one binary would mean two tick counters that each see a fraction of
 * the ticks, so the ARP timer would run at some unpredictable fraction of
 * ARP_TMR_INTERVAL depending on which loop the application was in. That is the
 * kind of defect that works on the bench and fails in the field, which is
 * exactly the class this project is built to refuse.
 *
 * TIMING
 *
 * The platform's SCU-timer ISR sets TcpFastTmrFlag every tick, and the tick is
 * 250 ms (platform_zynq.c loads XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ / 8 into a
 * timer clocked at CPU/2). That flag is the timeout unit for every caller: four
 * ticks is one second.
 *
 * There is no TCP anywhere in this application, so nothing else wants the flag
 * and tcp_fasttmr() is deliberately not called.
 *
 * Careful reading the vendor's comment above RESET_RX_CNTR_LIMIT in
 * platform_zynq.c — it claims the Rx-path workaround fires "every 100
 * milliseconds", but 400 ticks at 250 ms is 100 seconds. The comment is stale;
 * the tick is what the timer load value says it is.
 */

#include "net_pump.h"

#include "lwip/etharp.h"
#include "netif/xadapter.h"

/* Set by the platform's timer ISR every 250 ms. */
extern volatile int TcpFastTmrFlag;

#define NET_TICK_MS       250U
#define NET_ARP_TMR_TICKS (ARP_TMR_INTERVAL / NET_TICK_MS)

u32_t
net_pump(struct netif *netif)
{
	static u32_t arp_ticks = 0U;
	u32_t tick = 0U;

	xemacif_input(netif);

	if (TcpFastTmrFlag) {
		TcpFastTmrFlag = 0;
		tick = 1U;

		if (++arp_ticks >= NET_ARP_TMR_TICKS) {
			arp_ticks = 0U;
			etharp_tmr();
		}
	}
	return tick;
}

void
net_pump_ms(struct netif *netif, u32_t ms)
{
	u32_t want = (ms + NET_TICK_MS - 1U) / NET_TICK_MS;
	u32_t seen = 0U;

	while (seen < want) {
		seen += net_pump(netif);
	}
}
