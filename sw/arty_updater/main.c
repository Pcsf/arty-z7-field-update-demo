/*
 * main.c — the Case B field updater application.
 *
 * ONE binary, TWO roles. The same ELF goes into both the golden and the update
 * BOOT.BIN, and it learns which one it is at runtime from the MultiBoot slot
 * index -- never from a build flag, because a build flag is a second source of
 * truth for a fact the hardware already knows, and this project has spent
 * enough time on facts that lived in more than one place.
 *
 *   GOLDEN (slot 0)  the updater IS the mission. Bring up the network, wait for
 *                    an operator, run the pipeline, redirect.
 *   UPDATE (slot !0) prove this image is healthy and commit. The arbiter has
 *                    already counted this attempt; failing to commit is what
 *                    eventually sends the board back to golden.
 *
 * ONE LOOP, BOTH ROLES, AND WHY THE OPERATOR IS ON THE WIRE
 *
 * The deployment has Ethernet and no serial console, so the operator interface
 * is a UDP command channel -- doc/icd_control_channel.md. The serial menu
 * survives as a local diagnostic and reaches the SAME handlers; there is no
 * second implementation of any verb.
 *
 * Both roles run serve() and both bring the network up. An update image that
 * cannot be queried or sent back to golden is, on an Ethernet-only board, as
 * unreachable as a brick even when its flash is perfect.
 *
 * serve() never blocks. The old golden loop sat in inbyte() waiting for a
 * keystroke and serviced nothing while it waited, so on a live LAN the EMAC ISR
 * kept allocating pbufs that nothing drained and the pool exhausted -- measured
 * at 29 x "unable to alloc pbuf in recv_handler" in 8 seconds. A board sitting
 * in golden became progressively less able to accept the update it exists to
 * accept. The loop now pumps the stack every iteration and
 * touches the UART only when a byte is already waiting.
 *
 * ON THE WATCHDOG, AND WHY IT IS ARMED HERE
 *
 * The FSBL stops the watchdog unconditionally before handoff (main.c:759 of the
 * stock FSBL), so the arbiter cannot arm one that survives into this
 * application -- see doc/icd_boot_arbiter.md § The Watchdog Is Armed By The
 * Application. It is therefore armed here, as the first substantive action in
 * the update role, before the console, before lwIP, before anything that could
 * plausibly hang.
 *
 * The residual gap is real and is stated rather than hidden: an update image
 * that dies in C runtime startup, before reaching main(), is not covered. That
 * window is a few hundred instructions of code that is identical in the golden
 * image, which is already known to boot.
 */

#include <stdio.h>

#include "xparameters.h"
#include "netif/xadapter.h"

#include "platform.h"
#include "platform_config.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xuartps_hw.h"
#include "xwdtps.h"

#include "lwip/init.h"
#include "lwip/ip_addr.h"

#include "updater.h"
#include "ctrl_channel.h"
#include "net_pump.h"
#include "multiboot.h"
#include "qspi_flash.h"
#include "boot_state.h"

/*
 * FAULT INJECTION: an update image that hangs before commit.
 *
 * Build the v2 image with -DUPDATER_FAULT_HANG to get an update image that
 * comes up, proves its payload healthy, and then deliberately never commits.
 * The watchdog then resets it, the arbiter counts 1 -> 2 -> 3, and at
 * BOOT_ATTEMPTS_MAX the board falls back to golden. That is the whole safety
 * argument of Case B and it is not proven until it has been watched.
 */
#ifndef UPDATER_FAULT_HANG
#define UPDATER_FAULT_HANG 0
#endif

/* Where update.bin lands. 16 MiB up, clear of this application, and large
 * enough for a whole slot -- the ICD requires the buffer to be at least the
 * slot size so an oversized image is refused rather than truncated. */
#define DL_BASE   0x01000000U
#define DL_MAX    (QSPI_WRITE_CEIL - QSPI_WRITE_FLOOR)   /* 8.94 MB */

/* Static addressing; there is no DHCP client in this build. */
#define BOARD_IP_A 192
#define BOARD_IP_B 168
#define BOARD_IP_C   1
#define BOARD_IP_D  10
#define TFTP_SRV_A 192
#define TFTP_SRV_B 168
#define TFTP_SRV_C   1
#define TFTP_SRV_D 215

/* 15 s, per the ICD. Long enough that a TFTP fetch over a slow link does not
 * trip it, short enough that three attempts do not test anyone's patience. */
#define WDT_TIMEOUT_S   15U
#define WDT_PRESCALER   4096U
#define WDT_CRV_SHIFT   12U

static struct netif server_netif;
struct netif *echo_netif;    /* platform_zynq.c externs this by name */

static XWdtPs wdt;
static int    wdt_running = 0;

/* Set once in main(), read by the verb handlers. The slot index is the single
 * source of truth for the role; is_golden is derived from it, not stored
 * alongside it. */
static ip_addr_t tftp_server;
static u32       my_slot;
static int       is_golden;
static int       net_ok;    /* the interface came up; safe to pump   */
static int       ctrl_up;   /* the control port bound; verbs reachable */

/*
 * Counter maths lifted from the FSBL's ConvertTime_WdtCounter(), in integer
 * form: counts = seconds * clk / prescaler, then >> 12 for the CRV field.
 * Integer rather than the FSBL's double, because pulling in soft-float for one
 * constant at boot is not a trade worth making.
 */
static u32
wdt_crv(u32 seconds)
{
	u32 counts = (u32)(((u64)seconds * (u64)XPAR_XWDTPS_0_WDT_CLK_FREQ_HZ)
	                   / (u64)WDT_PRESCALER);
	return counts >> WDT_CRV_SHIFT;
}

static void
wdt_arm(void)
{
	XWdtPs_Config *cfg = XWdtPs_LookupConfig(XPAR_XWDTPS_0_DEVICE_ID);

	if (cfg == NULL) {
		xil_printf("UPD: WATCHDOG ABSENT - fallback path is NOT armed\r\n");
		return;
	}
	if (XWdtPs_CfgInitialize(&wdt, cfg, cfg->BaseAddress) != XST_SUCCESS) {
		xil_printf("UPD: WATCHDOG INIT FAILED - fallback path is NOT armed\r\n");
		return;
	}

	XWdtPs_SetControlValue(&wdt, XWDTPS_CLK_PRESCALE, XWDTPS_CCR_PSCALE_4096);
	XWdtPs_SetControlValue(&wdt, XWDTPS_COUNTER_RESET, wdt_crv(WDT_TIMEOUT_S));

	/* Reset output, not interrupt: a hung image must be RESET so the BootROM
	 * runs again and the arbiter gets another say. An interrupt would need
	 * working software to handle it, which is exactly what is in question. */
	XWdtPs_EnableOutput(&wdt, XWDTPS_RESET_SIGNAL);
	XWdtPs_Start(&wdt);
	XWdtPs_RestartWdt(&wdt);
	wdt_running = 1;

	xil_printf("UPD: watchdog armed, %u s\r\n", (unsigned)WDT_TIMEOUT_S);
}

static void
wdt_kick(void)
{
	if (wdt_running) {
		XWdtPs_RestartWdt(&wdt);
	}
}

static void
net_up(void)
{
	ip_addr_t ipaddr, netmask, gw;
	unsigned char mac[] = { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };

	echo_netif = &server_netif;

	IP4_ADDR(&ipaddr,  BOARD_IP_A, BOARD_IP_B, BOARD_IP_C, BOARD_IP_D);
	IP4_ADDR(&netmask, 255, 255, 255, 0);
	IP4_ADDR(&gw,      BOARD_IP_A, BOARD_IP_B, BOARD_IP_C, 1);
	IP4_ADDR(&tftp_server, TFTP_SRV_A, TFTP_SRV_B, TFTP_SRV_C, TFTP_SRV_D);

	lwip_init();
	if (!xemac_add(echo_netif, &ipaddr, &netmask, &gw, mac,
	               PLATFORM_EMAC_BASEADDR)) {
		/* net_ok stays 0, so serve() will not pump an interface that was
		 * never added -- and the serial menu still works, which is the only
		 * way to diagnose a board in this state. */
		xil_printf("UPD: could not add network interface - serial only\r\n");
		return;
	}
	netif_set_default(echo_netif);
	platform_enable_interrupts();
	netif_set_up(echo_netif);
	net_ok = 1;

	xil_printf("UPD: board %d.%d.%d.%d, server %d.%d.%d.%d\r\n",
	           BOARD_IP_A, BOARD_IP_B, BOARD_IP_C, BOARD_IP_D,
	           TFTP_SRV_A, TFTP_SRV_B, TFTP_SRV_C, TFTP_SRV_D);

	ctrl_up = ctrl_open(echo_netif);
	if (ctrl_up) {
		xil_printf("UPD: control channel on UDP %u\r\n", (unsigned)CTRL_PORT);
	} else {
		/* Not fatal, and not silent: everything else works, but the only
		 * operator interface on an Ethernet-only board is gone. */
		xil_printf("UPD: CONTROL CHANNEL FAILED TO BIND - serial only\r\n");
	}
}

/*
 * Everything that must keep happening while something slow runs: service the
 * stack, service the watchdog. One definition, used by the main loop and passed
 * into the pipeline, so there is no window in which the board is doing work and
 * not doing these.
 *
 * The pipeline is exactly such a window: steps 4 to 8 run ~20 s. Without a poll
 * the control channel goes deaf while datagrams pile up in the MAC and are
 * delivered afterwards -- a GOLDEN sent during a flash write executes tens of
 * seconds later and condemns the update that was just installed. See updater.h.
 */
static void
serve_poll(void)
{
	if (net_ok) {
		(void)net_pump(echo_netif);
	}
	wdt_kick();
}

static void
print_state(const boot_state_t *st, bs_result_t br)
{
	xil_printf("UPD: boot state: %s, update_present=%u attempts=%u\r\n",
	           bs_result_str(br), (unsigned)st->update_present,
	           (unsigned)st->boot_attempts);

	/* The advice, on the console only. bs_result_str() has to stay short
	 * because STATUS carries it inside a bounded reply; there is no such budget
	 * here, and this is the one place the sentence is actionable. */
	if (br == BS_LEGACY) {
		xil_printf("UPD: the record predates the installed_version field. The "
		           "slot may still hold a good\r\n"
		           "     image, but this board can no longer vouch for it - "
		           "re-run UPDATE to restore the record.\r\n");
	}
}

/* ---- Verb handlers ------------------------------------------------------ *
 *
 * Reached from the wire and from the serial menu, identically. Each builds one
 * reply line and hands it to ctrl_reply_send(), which echoes it to the console
 * and sends it to the requester if there is one -- so the console log shows
 * exactly what the network was told.
 */

static void
verb_err(ctrl_verb_t v, const char *why)
{
	ctrl_reply_t r;

	ctrl_reply_init(&r);
	ctrl_reply_str(&r, "ERR ");
	ctrl_reply_str(&r, ctrl_verb_str(v));
	ctrl_reply_str(&r, " ");
	ctrl_reply_str(&r, why);
	ctrl_reply_send(&r);
}

static void
do_status(void)
{
	ctrl_reply_t r;
	boot_state_t st;
	bs_result_t  br;

	br = boot_state_read(&st);

	ctrl_reply_init(&r);
	ctrl_reply_str(&r, "OK STATUS role=");
	ctrl_reply_str(&r, is_golden ? "GOLDEN" : "UPDATE");
	ctrl_reply_str(&r, " slot=");
	ctrl_reply_hex32(&r, my_slot);
	ctrl_reply_str(&r, " update_present=");
	ctrl_reply_u32(&r, st.update_present);
	ctrl_reply_str(&r, " boot_attempts=");
	ctrl_reply_u32(&r, st.boot_attempts);
	/* Two different claims, deliberately both shown. `version` is measured from
	 * the running payload's AXI register; `installed` is what the manifest said
	 * about whatever is sitting in the update slot, which nothing on the board
	 * can check until it boots. Reporting only one of them leaves the board able
	 * to say what it is running and never what it is about to run. */
	ctrl_reply_str(&r, " version=");
	ctrl_reply_hex32(&r, updater_payload_version());
	ctrl_reply_str(&r, " installed=");
	ctrl_reply_hex32(&r, st.installed_version);
	ctrl_reply_str(&r, " state=");
	ctrl_reply_str(&r, bs_result_str(br));
	ctrl_reply_send(&r);
}

static void
do_update(int force)
{
	ctrl_reply_t r;
	upd_result_t ur;

	/* Answer before the ~60 s pipeline starts. A single reply a minute after
	 * the request is indistinguishable, from the operator's side, from a board
	 * that ignored it. */
	ctrl_reply_init(&r);
	ctrl_reply_str(&r, force ? "OK UPDATE started (forced)"
	                         : "OK UPDATE started");
	ctrl_reply_send(&r);

	ur = updater_run(echo_netif, &tftp_server, (u8 *)DL_BASE, DL_MAX, force,
	                 serve_poll);

	ctrl_reply_init(&r);
	if (ur == UPD_OK) {
		/* Deliberately does NOT redirect. Step 9 stays a separate act, so the
		 * commit can be seen to land before anything reboots -- and so the
		 * operator chooses when the board goes down. That is SWITCH. */
		ctrl_reply_str(&r, "OK UPDATE installed - send SWITCH to boot it");
	} else if (ur == UPD_SKIPPED_SAME) {
		/* OK, not ERR: the operator asked for a state and the board is already
		 * in it. Nothing failed and nothing needs retrying. */
		ctrl_reply_str(&r, "OK UPDATE skipped - the slot already holds this "
		                   "image");
	} else {
		const char *detail = updater_last_detail();

		ctrl_reply_str(&r, "ERR UPDATE ");
		ctrl_reply_str(&r, upd_result_str(ur));
		/* The wire is the only channel this deployment has. Whatever the
		 * console was told, tell the operator too. */
		if (detail[0] != '\0') {
			ctrl_reply_str(&r, " - ");
			ctrl_reply_str(&r, detail);
		}
	}
	ctrl_reply_send(&r);
}

/*
 * SWITCH and GOLDEN are exact inverses on one field, which is what makes them a
 * pair: SWITCH writes boot_attempts = 0 and the arbiter redirects; GOLDEN writes
 * boot_attempts = BOOT_ATTEMPTS_MAX and the arbiter does not. Neither touches
 * update_present -- clearing it would also stick, but SWITCH could not undo it,
 * because SWITCH refuses when no update is installed.
 */
static bs_result_t
set_attempts(u32 attempts)
{
	boot_state_t st;

	(void)boot_state_read(&st);
	st.boot_attempts = attempts;
	return boot_state_write(&st);
}

/* Reply, let it reach the wire, then reset. Without the drain the operator's
 * client times out on a command that worked perfectly. */
static void
reply_then_reset(ctrl_verb_t v, const ctrl_reply_t *r, u32 offset)
{
	ctrl_reply_send(r);
	ctrl_drain();
	(void)multiboot_to(offset);
	/* Only reached if the offset was refused. The boot-state write already
	 * happened, so say which verb is left half-done rather than just "refused". */
	verb_err(v, "redirect refused - state written, still here");
}

static void
do_switch(void)
{
	ctrl_reply_t r;
	boot_state_t st;
	bs_result_t  br;

	(void)boot_state_read(&st);
	if (!st.update_present) {
		verb_err(CTRL_VERB_SWITCH, "no update installed");
		return;
	}

	br = set_attempts(0U);
	if (br != BS_OK) {
		/* No reset. Resetting without the record written would boot straight
		 * back into the state that prompted the command. */
		verb_err(CTRL_VERB_SWITCH, bs_result_str(br));
		return;
	}

	ctrl_reply_init(&r);
	ctrl_reply_str(&r, "OK SWITCH rebooting into update slot");
	reply_then_reset(CTRL_VERB_SWITCH, &r, QSPI_WRITE_FLOOR);
}

static void
do_golden(void)
{
	ctrl_reply_t r;
	boot_state_t st;

	(void)boot_state_read(&st);

	/* Nothing to condemn on a board with no update installed -- and a
	 * boot-state write is a full 64 KB sector erase, so it is not free. */
	if (st.update_present) {
		bs_result_t br = set_attempts((u32)BOOT_ATTEMPTS_MAX);

		if (br != BS_OK) {
			verb_err(CTRL_VERB_GOLDEN, bs_result_str(br));
			return;
		}
	}

	ctrl_reply_init(&r);
	ctrl_reply_str(&r, "OK GOLDEN rebooting into golden");
	reply_then_reset(CTRL_VERB_GOLDEN, &r, 0U);
}

/*
 * Role gating lives here, in the application, because roles are the
 * application's business -- ctrl_channel.c owns the wire and knows nothing
 * about slots, flash or the pipeline.
 */
static void
dispatch(ctrl_verb_t v)
{
	switch (v) {
	case CTRL_VERB_STATUS:
		do_status();
		break;

	case CTRL_VERB_UPDATE:
	case CTRL_VERB_UPDATE_FORCE:
		/* Refused in the update role: the capability is already reachable as
		 * GOLDEN then UPDATE, and allowing it directly means an image
		 * rewriting the slot it was loaded from. See the ICD § Role gating.
		 *
		 * FORCE does not weaken this gate or any other. It changes exactly one
		 * decision -- whether a declared downgrade may install -- and shares
		 * every refusal above it, because a flag that quietly widened its own
		 * scope is the thing an override must never be. */
		if (!is_golden) {
			verb_err(v, "not available in the update role");
		} else if (!net_ok) {
			/* Refuse rather than let the pipeline pump an interface that was
			 * never added. Only reachable from the serial menu -- with no
			 * network there is nothing to send a verb over. */
			verb_err(v, "network is down");
		} else {
			do_update(v == CTRL_VERB_UPDATE_FORCE);
		}
		break;

	case CTRL_VERB_SWITCH:
		if (is_golden) {
			do_switch();
		} else {
			verb_err(v, "already running the update");
		}
		break;

	case CTRL_VERB_GOLDEN:
		do_golden();
		break;

	default:
		break;
	}
}

/* ---- The loop ----------------------------------------------------------- */

static void
menu_print(void)
{
	if (is_golden) {
		xil_printf("\r\n[s] status  [u] update  [w] switch to update  "
		           "[g] back to golden  [r] soft reset\r\n> ");
	} else {
		xil_printf("\r\n[s] status  [g] back to golden  [r] soft reset\r\n> ");
	}
}

/*
 * Non-blocking console. XUartPs_IsReceiveData() is a register read of the
 * status bit; inbyte() is only called once a byte is known to be waiting, so
 * the loop never stops servicing the stack to wait for a human.
 */
static int
console_byte(int *out)
{
	if (!XUartPs_IsReceiveData(STDIN_BASEADDRESS)) {
		return 0;
	}
	*out = inbyte();
	return 1;
}

static void
menu_key(int c)
{
	ctrl_verb_t v = CTRL_VERB_NONE;

	xil_printf("%c\r\n", c);

	switch (c) {
	case 's': case 'S': v = CTRL_VERB_STATUS; break;
	case 'u': case 'U': v = CTRL_VERB_UPDATE; break;
	case 'w': case 'W': v = CTRL_VERB_SWITCH; break;
	case 'g': case 'G': v = CTRL_VERB_GOLDEN; break;

	case 'r': case 'R':
		/* Local diagnostic only, and deliberately NOT the GOLDEN verb: a plain
		 * soft reset changes no state. It is the recorded recovery for a
		 * wedged stack. */
		xil_printf("UPD: resetting\r\n");
		(void)multiboot_to(0U);
		return;

	default:
		menu_print();
		return;
	}

	ctrl_begin_local();
	dispatch(v);
	ctrl_done();
	menu_print();
}

/*
 * Both roles, one loop. Never blocks: every iteration services the stack, kicks
 * the watchdog (a no-op in the golden role, which does not arm one), runs at
 * most one pending verb, and reads the UART only if a byte is already there.
 */
static void
serve(void)
{
	menu_print();

	for (;;) {
		ctrl_verb_t v;
		int         c;

		serve_poll();

		v = ctrl_take_pending();
		if (v != CTRL_VERB_NONE) {
			dispatch(v);
			ctrl_done();
			menu_print();
		}

		if (console_byte(&c)) {
			menu_key(c);
		}
	}
}

/* ---- Roles -------------------------------------------------------------- */

static void
run_golden(void)
{
	boot_state_t st;
	bs_result_t  br;

	xil_printf("\r\n=== GOLDEN - field updater ===\r\n");

	br = boot_state_read(&st);
	print_state(&st, br);

	if (st.update_present && st.boot_attempts >= BOOT_ATTEMPTS_MAX) {
		/* The arbiter sent us here rather than to the update slot. Say so
		 * plainly: this is the fallback report the ICD promises an operator.
		 * Note it is also what a GOLDEN verb leaves behind, by design. */
		xil_printf("UPD: FALLBACK - the installed update failed %u attempts\r\n",
		           (unsigned)BOOT_ATTEMPTS_MAX);
	} else if (!st.update_present) {
		xil_printf("UPD: no valid update installed - serving\r\n");
	}

	net_up();
	serve();
}

static void
run_update(u32 slot)
{
	boot_state_t st;
	bs_result_t  br;

	xil_printf("\r\n=== UPDATE - slot index 0x%08x ===\r\n", (unsigned)slot);

	br = boot_state_read(&st);
	print_state(&st, br);

	/*
	 * Health first, network second. The probe blocks for ten seconds and does
	 * not pump, so bringing the stack up before it would create the very
	 * starvation this application was just rewritten to avoid. Ten seconds of
	 * being unreachable at boot is not a cost worth paying to avoid.
	 */
	xil_printf("UPD: proving payload health\r\n");
	if (!updater_payload_healthy(10U, serve_poll)) {
		/*
		 * Do NOT commit. Stop kicking and let the watchdog reset us: the
		 * arbiter will count the attempt and, at BOOT_ATTEMPTS_MAX, route back
		 * to golden. Deliberately not calling multiboot_to(0) here -- letting
		 * the counter do it exercises the same path a genuine hang takes.
		 */
		xil_printf("UPD: payload UNHEALTHY - not committing, awaiting watchdog\r\n");
		for (;;) { }
	}

#if UPDATER_FAULT_HANG
	xil_printf("UPD: FAULT INJECTION - hanging before commit\r\n");
	for (;;) { }
#endif

	st.boot_attempts = 0U;
	br = boot_state_write(&st);
	if (br != BS_OK) {
		/* Cannot record the commit. Do not pretend it happened -- keep running
		 * so the board stays up and reachable, and say so loudly. */
		xil_printf("UPD: COMMIT FAILED (%s) - this image will be retried\r\n",
		           bs_result_str(br));
	} else {
		xil_printf("UPD: COMMITTED - attempts cleared, this image is now steady\r\n");
	}

	net_up();

	xil_printf("UPD: running. Servicing watchdog, answering STATUS and GOLDEN.\r\n");
	serve();
}

int
main(void)
{
	qspi_result_t qr;

	/*
	 * Slot first, watchdog second, everything else after. Reading the slot
	 * index is a single masked register read, so arming still happens within a
	 * few instructions of entry -- and arming in the golden role would be
	 * wrong, since golden is the safe state and resetting a correctly
	 * behaving board is not a safety feature.
	 */
	my_slot   = multiboot_slot_index();

	/*
	 * SAME TEST AS THE ARBITER'S, AND FOR THE SAME REASON — see fsbl_hooks.c.
	 * Exactly one MULTIBOOT_ADDR value means "I am the update"; everything else
	 * means golden, which is the safe role.
	 *
	 * KEEP THIS IN STEP WITH THE ARBITER. When only the arbiter's copy of the
	 * test was corrected, the FSBL announced `ARB: golden (slot 512)` and this
	 * line still brought the board up as role=UPDATE, on golden's own bytes with
	 * the watchdog armed. One rule written twice is the defect; fixing one copy
	 * moves it rather than removing it.
	 *
	 * Both copies now derive the constant from the slot base instead of
	 * writing it out, so moving the update slot moves both tests with it.
	 */
	is_golden = (my_slot != (QSPI_WRITE_FLOOR / MB_SLOT_UNIT));

	if (!is_golden) {
		wdt_arm();
	}

	init_platform();

	xil_printf("\r\n--- field updater ---\r\n");

	qr = qspi_flash_init();
	if (qr != QSPI_OK) {
		/* Without flash there is no boot state and no slot to write. Report and
		 * stop; in the update role the watchdog will reset us and the attempt
		 * counter will eventually fall back to golden. */
		xil_printf("UPD: QSPI init failed (%s)\r\n", qspi_result_str(qr));
		for (;;) {
			wdt_kick();
		}
	}

	if (is_golden) {
		run_golden();
	} else {
		run_update(my_slot);
	}

	return 0;
}
