/*
 * TFTP client app entry point.
 *
 * Overlaid onto the Vitis "lwIP Echo Server" template. The template is used for
 * its network bring-up and nothing else: xemac_add(), the SCU-timer platform
 * layer, and the RTL8211E link path cost a full session to get right on this
 * board (see README § the two PHY workarounds), and re-deriving them from an
 * Empty Application would buy nothing. This file replaces the template's
 * main.c; echo.c stays in the project and is never called, so the echo server's
 * TCP listener is never opened.
 *
 * TWO SYMBOLS THIS FILE MUST KEEP
 *   - main()        obviously.
 *   - echo_netif    platform_zynq.c externs it (line 79) and dereferences it in
 *                   the timer ISR for xemacpsif_resetrx_on_no_rxdata() and
 *                   eth_link_detect(). Rename it and the app fails to link with
 *                   an undefined reference from a vendor file you did not
 *                   touch. The name is inherited, not descriptive.
 *
 * The link monitor is disabled via ETH_LINK_DETECT_INTERVAL in our platform.h —
 * on this board it destroys the link it is watching. Same override, same
 * reasons, as the echo app; that file carries the full write-up.
 */

#include <stdio.h>

#include "xparameters.h"
#include "netif/xadapter.h"

#include "platform.h"
#include "platform_config.h"
#include "xil_printf.h"
#include "xil_cache.h"

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/etharp.h"

#include "tftp_client.h"


/*
 * Where fetched files land.
 *
 * A fixed DDR address rather than a static array, for two reasons. It mirrors
 * the register app's result block at 0x0080_0000, so the JTAG inspection route
 * in README works the same way here; and it is the shape the updater needs,
 * where update.bin is fetched to a known address and handed to the QSPI
 * programmer without a copy. 0x0100_0000 is 16 MiB up — comfortably clear of this app,
 * which lscript.ld places near the bottom of DDR, and the board has 512 MiB.
 *
 * The cap is what makes TFTP_ERR_TOOBIG meaningful: 1 MiB is far more than the
 * probe files need and far less than a BOOT.BIN.
 */
#define TFTP_DL_BASE   0x01000000U
#define TFTP_DL_MAX    (1U * 1024U * 1024U)

/*
 * Board and server addressing. Two benches, one switch.
 *
 *   TFTP_BENCH_DIRECT 0 — LAN MODE (default). The board plugs into the house
 *       router/switch and the host serves from wlan0. Simplest to live with:
 *       no cable to move, the host keeps its normal networking, and the board
 *       is reachable from anywhere on the LAN.
 *
 *   TFTP_BENCH_DIRECT 1 — POINT-TO-POINT. The board plugs straight into the
 *       host's enp196s0 on a private subnet (host/10-fpga-bench.network).
 *       Latency drops from 2-1300 ms through the AP to ~0.15 ms, the bench is
 *       isolated from the house LAN, and cable-pull fault injection becomes
 *       trivially controllable.
 *
 * Both work. For a long time only the second appeared to, and the reason had
 * nothing to do with either network: nothing was driving lwIP's ARP timer, so a
 * single lost ARP request at boot was permanent and no application-level retry
 * could recover it. See the long comment on tftp_pump() in tftp_client.c. With
 * that fixed, LAN mode resolves unaided too, which is why it is the default
 * again.
 *
 * The gateway matters only in LAN mode; in point-to-point the server is on-link
 * and it is never consulted.
 */
#define TFTP_BENCH_DIRECT   0

#if TFTP_BENCH_DIRECT
  /* Private bench subnet — deliberately NOT wlan0's 192.168.1.0/24, since two
   * interfaces holding routes to one prefix is ambiguous and muddles ARP. */
  #define BOARD_IP_A   192
  #define BOARD_IP_B   168
  #define BOARD_IP_C    10
  #define BOARD_IP_D    10

  #define TFTP_SRV_A   192
  #define TFTP_SRV_B   168
  #define TFTP_SRV_C    10
  #define TFTP_SRV_D     1
#else
  /* House LAN. The server is the development host on wlan0. */
  #define BOARD_IP_A   192
  #define BOARD_IP_B   168
  #define BOARD_IP_C     1
  #define BOARD_IP_D    10

  #define TFTP_SRV_A   192
  #define TFTP_SRV_B   168
  #define TFTP_SRV_C     1
  #define TFTP_SRV_D   215
#endif

/*
 * Both probe files are fetched, back to back, in one boot.
 *
 * probe_1024.txt is not redundant. Its length is an exact multiple of the block
 * size, so the transfer ends with a DATA packet carrying ZERO bytes — the case
 * a client silently hangs on if "payload < 512" is its only terminator. And
 * running two transfers in one session is what proves per-transfer state really
 * resets, which is what the updater depends on when it fetches update.sha and
 * then update.bin.
 *
 * The expected sizes are asserted by host/setup_tftp.sh when it generates the
 * files, so a mismatch here means the transfer is wrong, not the fixture.
 */
static const struct {
	const char *name;
	u32_t       expect;
	int         dump;      /* print the body? only worth it for the first */
} fetches[] = {
	{ "probe.txt",      768U,  1 },
	{ "probe_1024.txt", 1024U, 0 },
};

#define N_FETCHES  (sizeof(fetches) / sizeof(fetches[0]))

static struct netif server_netif;
struct netif *echo_netif;     /* see the header comment — the name is load-bearing */

static void
print_ip(const char *msg, ip_addr_t *ip)
{
	xil_printf("%s%d.%d.%d.%d\r\n", msg,
	           ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip));
}

/*
 * Dump fetched bytes as text. Printed a byte at a time rather than with %s
 * because nothing guarantees the file is NUL-terminated — it is whatever the
 * host put there. Non-printables show as '.' so a binary file fetched by
 * mistake is obvious instead of shredding the terminal.
 */
static void
print_payload(const u8_t *p, u32_t len)
{
	u32_t i;

	for (i = 0U; i < len; i++) {
		u8_t c = p[i];

		if (c == '\n') {
			/* The file holds a bare LF, which is correct for the file and wrong
			 * for the terminal: a serial console in raw mode does not translate,
			 * so LF alone moves down without returning to column 0 and the dump
			 * comes out as a staircase. Emit CR LF for display only — the bytes
			 * in DDR are untouched. */
			xil_printf("\r\n");
		} else if (c == '\r' || c == '\t' || (c >= 0x20U && c < 0x7FU)) {
			xil_printf("%c", c);
		} else {
			xil_printf(".");
		}
	}
	xil_printf("\r\n");
}

int
main(void)
{
	ip_addr_t ipaddr, netmask, gw, server;
	u8_t     *dst = (u8_t *)TFTP_DL_BASE;
	u32_t     i;
	u32_t     passed = 0U;

	/* Unique per board; same value the echo app used, and only one board is
	 * ever on this segment. */
	unsigned char mac_ethernet_address[] = { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };

	echo_netif = &server_netif;

	init_platform();

	IP4_ADDR(&ipaddr,  BOARD_IP_A, BOARD_IP_B, BOARD_IP_C, BOARD_IP_D);
	IP4_ADDR(&netmask, 255, 255, 255, 0);
	IP4_ADDR(&gw,      BOARD_IP_A, BOARD_IP_B, BOARD_IP_C, 1);
	IP4_ADDR(&server,  TFTP_SRV_A, TFTP_SRV_B, TFTP_SRV_C, TFTP_SRV_D);

	xil_printf("\r\n--- TFTP client ---\r\n");

	lwip_init();

	if (!xemac_add(echo_netif, &ipaddr, &netmask, &gw,
	               mac_ethernet_address, PLATFORM_EMAC_BASEADDR)) {
		xil_printf("Error adding N/W interface\r\n");
		return -1;
	}
	netif_set_default(echo_netif);

	platform_enable_interrupts();
	netif_set_up(echo_netif);

	print_ip("Board IP  : ", &ipaddr);
	print_ip("Netmask   : ", &netmask);
	print_ip("Gateway   : ", &gw);
	print_ip("TFTP srv  : ", &server);
	xil_printf("Buffer    : 0x%08x (%d bytes max)\r\n", TFTP_DL_BASE, TFTP_DL_MAX);

	for (i = 0U; i < N_FETCHES; i++) {
		u32_t         got = 0U;
		tftp_result_t r;

		xil_printf("\r\nGET %s (expect %d bytes)...\r\n",
		           fetches[i].name, (int)fetches[i].expect);

		/*
		 * Poison the buffer before every fetch.
		 *
		 * DDR survives 'rst -processor' and a bitstream reload — nothing in the
		 * run sequence clears it. So a buffer holding a PREVIOUS run's
		 * successful download is indistinguishable, to anyone inspecting it
		 * over JTAG, from a fresh one. That is a verification trap, not a
		 * correctness bug: the app would report the failure honestly on the
		 * console while `mrd` showed perfect data from an hour ago.
		 *
		 * Overwriting first makes the JTAG route trustworthy — bytes at
		 * TFTP_DL_BASE are either 0xA5 filler or something this run fetched.
		 * Flushed so the DAP sees the poison too, for the same reason the
		 * success path flushes.
		 */
		memset(dst, 0xA5, fetches[i].expect + 64U);
		Xil_DCacheFlushRange((INTPTR)dst, fetches[i].expect + 64U);

		r = tftp_get(echo_netif, &server, fetches[i].name,
		             dst, TFTP_DL_MAX, &got);

		if (r != TFTP_OK) {
			xil_printf("  FAIL: %s (code %d)\r\n", tftp_result_str(r), (int)r);
			continue;
		}

		/* Flush before anyone reads this buffer over JTAG. The CPU's writes are
		 * in D-cache; 'mrd 0x01000000' goes through the DAP straight to DDR and
		 * would show stale memory without this. Harmless for the UART dump
		 * below, essential for the mrd route in README. */
		Xil_DCacheFlushRange((INTPTR)dst, got);

		if (got != fetches[i].expect) {
			xil_printf("  FAIL: got %d bytes, expected %d\r\n",
			           (int)got, (int)fetches[i].expect);
			continue;
		}

		xil_printf("  OK: %d bytes at 0x%08x\r\n", (int)got, TFTP_DL_BASE);
		passed++;

		if (fetches[i].dump) {
			xil_printf("---8<--- %s ---8<---\r\n", fetches[i].name);
			print_payload(dst, got);
			xil_printf("---8<--- end ---8<---\r\n");
		}
	}

	xil_printf("\r\n%d / %d transfers OK\r\n", (int)passed, (int)N_FETCHES);
	if (passed == N_FETCHES) {
		xil_printf("--- TFTP client: ALL CHECKS PASSED ---\r\n");
	} else {
		xil_printf("--- TFTP client: FAIL ---\r\n");
	}

	/* Idle rather than return. Returning from main() on bare metal lands in the
	 * C runtime's exit loop with the MMU still on, which is a confusing place to
	 * find the core parked when you attach with xsct. */
	while (1) {
		xemacif_input(echo_netif);
	}

	/* not reached */
	cleanup_platform();
	return 0;
}
