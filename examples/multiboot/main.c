/*
 * main.c — SLOT A: the golden image's payload.
 *
 * The second partition of the BOOT.BIN at QSPI offset 0x000000. It cannot be
 * proven over JTAG: with JP4 on JTAG the BootROM waits for a debugger instead of
 * fetching a boot image, so MULTIBOOT is structurally unobservable there. It has
 * to boot from flash.
 *
 * WHAT IT DOES
 *
 *   1. reports which slot the BootROM found it in, from MULTIBOOT_ADDR
 *   2. proves the SLCR unlock actually gates writes, using LOCKSTA
 *   3. checks the slot arithmetic and the two ways multiboot_arm() refuses
 *   4. redirects to 0x700000 and resets
 *
 * Step 2 is not decoration. Every reset register is SLCR-protected: with SLCR
 * locked the reset write is silently discarded and the board just carries on.
 * That is the most confusing failure this code can have, so the mechanism is
 * demonstrated rather than assumed — LOCKSTA reads 1 while locked and 0 after
 * the unlock key, on the console, before anything depends on it.
 *
 * THE REDIRECT ONLY FIRES FROM SLOT 0
 *
 * If this image is ever reached from somewhere other than offset 0, something
 * has gone wrong that a redirect would only compound. It reports and stops
 * instead.
 *
 * RECOVERY, BEFORE YOU NEED IT: move JP4 back to JTAG. That restores the
 * debugger workflow completely and unconditionally — the BootROM stops looking
 * at flash entirely, so no state written here can prevent it.
 */

#include "xil_printf.h"
#include "sleep.h"

#include "multiboot.h"

/* The flash map. */
#define SLOT_GOLDEN   0x000000U
#define SLOT_UPDATE   0x700000U

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
	u32         boot_raw;
	u32         boot_slot;
	u32         boot_offset;
	int         locked_before;
	mb_result_t r;

	/* FIRST, before anything arms the register: capture how we were booted.
	 * Every later step writes MULTIBOOT, so this reading is only available
	 * now. */
	boot_raw    = multiboot_read_raw();
	boot_slot   = multiboot_slot_index();
	boot_offset = multiboot_current_offset();

	xil_printf("\r\n\r\n=====================================\r\n");
	xil_printf("  SLOT A  --  GOLDEN @ 0x%06x\r\n", SLOT_GOLDEN);
	xil_printf("=====================================\r\n");

	xil_printf("MULTIBOOT_ADDR : 0x%08x\r\n", boot_raw);
	xil_printf("  slot index   : 0x%04x   (bits [12:0])\r\n", boot_slot);
	xil_printf("  above field  : 0x%08x   (bits [31:13] -- undocumented,\r\n",
	           boot_raw & ~(u32)MB_ADDR_MASK);
	xil_printf("                            set by the BootROM, ignored here)\r\n");
	xil_printf("booted from    : 0x%06x  (0x%08x & 0x1FFF) * 0x8000\r\n",
	           boot_offset, boot_raw);

	xil_printf("\r\n1. Which slot the BootROM used\r\n");
	/* MASKED, not raw. A normal QSPI boot from offset 0 leaves 0x0000C000 in
	 * this register on this silicon, so `boot_raw == 0` is false on a perfectly
	 * good golden boot -- see multiboot.h. The slot index is the only part of
	 * the register that answers the question. */
	check("slot index is 0 -- found at offset 0, not by a fallback search",
	      boot_slot == 0U);
	check("derived offset is the golden slot", boot_offset == SLOT_GOLDEN);

	/* ---- 2. the SLCR unlock, demonstrated ------------------------------ */

	xil_printf("\r\n2. SLCR write protection\r\n");

	locked_before = multiboot_slcr_is_locked();
	xil_printf("    LOCKSTA at entry: %d\r\n", locked_before);

	multiboot_slcr_lock();
	xil_printf("    after lock key   0x767B767B -> LOCKSTA %d\r\n",
	           multiboot_slcr_is_locked());
	check("SLCR reports locked after the lock key",
	      multiboot_slcr_is_locked() != 0);

	multiboot_slcr_unlock();
	xil_printf("    after unlock key 0xDF0DDF0D -> LOCKSTA %d\r\n",
	           multiboot_slcr_is_locked());
	check("SLCR reports unlocked after the unlock key",
	      multiboot_slcr_is_locked() == 0);

	/* ---- 3. the arithmetic, and the two refusals ----------------------- */

	xil_printf("\r\n3. Slot arithmetic\r\n");

	r = multiboot_arm(SLOT_UPDATE);
	xil_printf("    arm(0x%06x) -> %s; MULTIBOOT_ADDR now 0x%08x\r\n",
	           SLOT_UPDATE, mb_result_str(r), multiboot_read_raw());
	check("arm(0x700000) accepted", r == MB_OK);
	check("register holds 0xE0 -- 0x700000 / 0x8000",
	      (multiboot_read_raw() & MB_ADDR_MASK) == 0xE0U);
	check("it reads back as the offset we asked for",
	      multiboot_current_offset() == SLOT_UPDATE);

	/* An offset that is not a whole number of slots cannot be named by a
	 * 13-bit index, and rounding it would boot something nobody chose. */
	r = multiboot_arm(SLOT_UPDATE + 4U);
	xil_printf("    arm(0x%06x) -> %s\r\n", SLOT_UPDATE + 4U, mb_result_str(r));
	check("unaligned offset is refused", r == MB_ERR_ALIGN);

	/* One slot past what 13 bits can name. Truncating would alias it onto a
	 * low slot -- very often slot 0 -- which looks like "the redirect didn't
	 * work" rather than like a bug. */
	r = multiboot_arm((MB_ADDR_MASK + 1U) * MB_SLOT_UNIT);
	xil_printf("    arm(0x%08x) -> %s\r\n",
	           (MB_ADDR_MASK + 1U) * MB_SLOT_UNIT, mb_result_str(r));
	check("out-of-range offset is refused", r == MB_ERR_RANGE);

	check("refusals left the armed value intact",
	      multiboot_current_offset() == SLOT_UPDATE);

	/* ---- 4. redirect --------------------------------------------------- */

	xil_printf("\r\n%d / %d checks passed\r\n",
	           (int)(checks_run - checks_failed), (int)checks_run);

	if (boot_slot != 0U) {
		xil_printf("\r\nNOT redirecting: this image did not come from slot 0.\r\n");
		xil_printf("Something already went wrong; a redirect would compound it.\r\n");
		xil_printf("--- SLOT A: HALTED ---\r\n");
		while (1) {
			;
		}
	}

	xil_printf("\r\n4. Redirecting to 0x%06x\r\n", SLOT_UPDATE);
	xil_printf("   MULTIBOOT_ADDR is armed; a soft reset re-enters the BootROM,\r\n");
	xil_printf("   which will fetch its boot image from there instead of 0.\r\n");
	xil_printf("   If the next banner says SLOT B, the redirect works.\r\n\r\n");

	{
		int i;

		for (i = 3; i > 0; i--) {
			xil_printf("   ... %d\r\n", i);
			sleep(1);
		}
	}

	xil_printf("   resetting now\r\n\r\n");

	r = multiboot_to(SLOT_UPDATE);

	/* Only reachable if multiboot_to() refused the offset before writing
	 * anything -- on the success path it never returns. */
	xil_printf("UNEXPECTED: multiboot_to returned: %s\r\n", mb_result_str(r));
	xil_printf("--- SLOT A: FAIL ---\r\n");

	while (1) {
		;
	}

	return 0;
}
