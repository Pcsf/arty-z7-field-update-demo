/*
 * main.c — SLOT B: the update image's payload.
 *
 * The second partition of the BOOT.BIN at QSPI offset 0x700000.
 * Reaching it at all is the proof: the BootROM only fetches from there when
 * MULTIBOOT_ADDR says so, and the only thing that wrote MULTIBOOT_ADDR is slot
 * A's redirect.
 *
 * WHY IT DOES NOT LINK multiboot.[ch]
 *
 * Deliberately self-contained. It needs one register read, and sharing the
 * module would mean either duplicating two files into this overlay or teaching
 * the build to import from two directories — both worse than eight lines of
 * Xil_In32. More to the point, slot B is the image that must NOT redirect: it
 * is the end of the chain, and not having multiboot_to() in scope at all is a
 * stronger guarantee of that than remembering not to call it.
 *
 * WHAT MAKES THIS EVIDENCE AND NOT A BANNER
 *
 * A banner string proves nothing — the same image flashed into both slots would
 * print it either way. What is checked here is the register: MULTIBOOT_ADDR
 * must read 0xE0, and 0xE0 * 0x8000 == 0x700000, which is this slot's address.
 * The number derives from where the image physically is, so it cannot be faked
 * by the image's contents.
 */

#include "xil_io.h"
#include "xil_printf.h"

/* xparameters_ps.h:168, xdevcfg_hw.h:62 — see multiboot.h for the provenance of
 * every constant used here. */
#define DEVCFG_BASE        0xF8007000U
#define DEVCFG_MULTIBOOT   0x02CU

/* pcap.h:45 and fsbl.h:431 */
#define MB_ADDR_MASK       0x1FFFU
#define MB_SLOT_UNIT       0x8000U

#define SLOT_UPDATE        0x700000U
#define SLOT_UPDATE_INDEX  (SLOT_UPDATE / MB_SLOT_UNIT)   /* 0xE0 */

static u32 checks_run;
static u32 checks_failed;

static void
check(const char *what, int ok)
{
	checks_run++;
	if (!ok) {
		checks_failed++;
	}
	xil_printf("  [%s] %s\r\n", ok ? "PASS" : "FAIL", what);
}

int
main(void)
{
	u32 raw    = Xil_In32(DEVCFG_BASE + DEVCFG_MULTIBOOT);
	u32 offset = (raw & MB_ADDR_MASK) * MB_SLOT_UNIT;

	xil_printf("\r\n\r\n=====================================\r\n");
	xil_printf("  SLOT B  --  UPDATE @ 0x%06x\r\n", SLOT_UPDATE);
	xil_printf("=====================================\r\n");

	xil_printf("MULTIBOOT_ADDR : 0x%08x\r\n", raw);
	xil_printf("booted from    : 0x%06x  (0x%08x & 0x1FFF) * 0x8000\r\n",
	           offset, raw);

	xil_printf("\r\nThe redirect\r\n");
	check("MULTIBOOT_ADDR index is 0xE0",
	      (raw & MB_ADDR_MASK) == SLOT_UPDATE_INDEX);
	check("which is 0x700000 -- this slot's address in the flash map",
	      offset == SLOT_UPDATE);
	check("so the BootROM was redirected here, not started here",
	      (raw & MB_ADDR_MASK) != 0U);

	xil_printf("\r\n%d / %d checks passed\r\n",
	           (int)(checks_run - checks_failed), (int)checks_run);

	if (checks_failed == 0U) {
		xil_printf("--- MULTIBOOT REDIRECT PROVEN ---\r\n");
		xil_printf("\r\nPower-cycle to return to slot A: a POR clears\r\n");
		xil_printf("MULTIBOOT_ADDR, so golden is the default by construction.\r\n");
	} else {
		xil_printf("--- SLOT B: FAIL ---\r\n");
	}

	/* End of the chain. No redirect from here — see the header comment. */
	while (1) {
		;
	}

	return 0;
}
