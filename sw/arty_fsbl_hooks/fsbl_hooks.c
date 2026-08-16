/*
 * fsbl_hooks.c — the Case B boot arbiter.
 *
 * Contract: doc/icd_boot_arbiter.md.
 *
 * This file is overlaid onto the generated Zynq-7000 FSBL at build time, so the
 * same hooked FSBL binary sits inside BOTH the golden and the update BOOT.BIN.
 * Which one it is running as is a runtime question, answered by the MultiBoot
 * slot index, never by a build flag.
 *
 * THREE THINGS THIS FILE MUST NOT DO
 *
 * 1. It must not read DEVCFG_MULTIBOOT raw. The register is 13 bits wide and a
 *    normal golden boot leaves 0x0000C000 in it, so `reg != 0` is TRUE on
 *    golden. Use multiboot_slot_index(), which masks.
 *
 * 2. It must not return anything except XST_SUCCESS. A non-success return makes
 *    image_mover.c call FsblFallback(), which calls Update_MultiBootRegister(),
 *    which INCREMENTS the very register this arbiter derives its slot from —
 *    corrupting the input to the next boot's decision. Every failure here
 *    degrades to "boot golden" instead, the safe terminal state.
 *
 * 3. It must not arm the watchdog. The FSBL calls XWdtPs_Stop() unconditionally
 *    at main.c:759, before handoff, so anything armed here is switched off a few
 *    hundred instructions later. The update APPLICATION arms it.
 *
 * ON BORROWING THE QSPI CONTROLLER
 *
 * The FSBL reads partitions through LQSPI linear mode (qspi.c); qspi_flash.c
 * drives the same controller in manual I/O mode. This hook runs at
 * image_mover.c:431, BEFORE PartitionMove(), so switching modes without putting
 * them back would break the boot that is still reading images through that
 * controller. The controller registers are saved on entry and restored on every
 * exit path, including the ones that redirect.
 */

#include <stdint.h>
#include <stddef.h>

#include "xil_io.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xstatus.h"
#include "xqspips_hw.h"

#include "boot_state.h"
#include "qspi_flash.h"
#include "multiboot.h"

/* GOLDEN is offset 0 by definition — where the BootROM looks with
 * MULTIBOOT_ADDR clear. UPDATE comes from qspi_flash.h rather than being written
 * again: the update slot base and the write-guard floor are the same fact. */
#define GOLDEN_BASE   0x000000U
#define UPDATE_BASE   QSPI_WRITE_FLOOR      /* 0x700000 */

/* The one MULTIBOOT_ADDR value that means "I am the update", derived from
 * UPDATE_BASE so moving the slot moves this with it. See the role test below
 * for why it is a single value and not a negation. */
#define UPDATE_SLOT_INDEX  (UPDATE_BASE / MB_SLOT_UNIT)   /* 224 */

#define QSPI_BASE     XPAR_XQSPIPS_0_BASEADDR

typedef struct {
	u32 cr;
	u32 er;
	u32 lqspi_cr;
} qspi_ctrl_save_t;

static void qspi_ctrl_save(qspi_ctrl_save_t *s)
{
	s->cr       = Xil_In32(QSPI_BASE + XQSPIPS_CR_OFFSET);
	s->er       = Xil_In32(QSPI_BASE + XQSPIPS_ER_OFFSET);
	s->lqspi_cr = Xil_In32(QSPI_BASE + XQSPIPS_LQSPI_CR_OFFSET);
}

static void qspi_ctrl_restore(const qspi_ctrl_save_t *s)
{
	/* Disable before rewriting mode bits, then re-enable — changing LQSPI_CR
	 * under a running controller leaves it in neither mode. */
	Xil_Out32(QSPI_BASE + XQSPIPS_ER_OFFSET, 0U);
	Xil_Out32(QSPI_BASE + XQSPIPS_LQSPI_CR_OFFSET, s->lqspi_cr);
	Xil_Out32(QSPI_BASE + XQSPIPS_CR_OFFSET, s->cr);
	Xil_Out32(QSPI_BASE + XQSPIPS_ER_OFFSET, s->er);
}

/* xil_printf rather than fsbl_printf: fsbl_printf is compiled out unless
 * FSBL_DEBUG is defined, and this arbiter must never make a silent decision. */
#define ARB(fmt, ...)   xil_printf("ARB: " fmt "\r\n", ##__VA_ARGS__)

u32 FsblHookBeforeBitstreamDload(void)
{
	qspi_ctrl_save_t save;
	boot_state_t     st;
	bs_result_t      br;
	qspi_result_t    qr;
	u32              slot;

	qspi_ctrl_save(&save);

	/* Force, not init: this borrows a controller the FSBL already configured,
	 * and the cached "already initialised" flag would skip re-applying manual
	 * mode if this hook ever ran twice in one boot. */
	qr = qspi_flash_force_reinit();
	if (qr != QSPI_OK) {
		ARB("qspi init failed (%s) - booting golden", qspi_result_str(qr));
		qspi_ctrl_restore(&save);
		return XST_SUCCESS;
	}

	/* Always yields a usable record: corrupt and flash-failure both come back
	 * as {update_present=0, boot_attempts=0}. */
	br = boot_state_read(&st);
	if (br != BS_OK) {
		ARB("boot state: %s", bs_result_str(br));
	}

	slot = multiboot_slot_index();

	/*
	 * ROLE IS DECIDED BY "AM I THE UPDATE?", NOT "AM I NOT GOLDEN?".
	 *
	 * When the image at the armed slot has a bad header the BootROM increments
	 * MULTIBOOT_ADDR looking for another one, and on a 16 MiB part that search
	 * runs off the end and WRAPS: slot 512 is offset 0x1000000, which is offset
	 * 0, which is golden. Testing `slot == 0` for golden therefore has the board
	 * running golden's bytes while calling itself the update — arming a watchdog,
	 * running a health probe and committing an update that was never installed,
	 * after which it refuses both UPDATE and SWITCH and cannot be updated at all.
	 *
	 * Exactly one value means update. Everything else means golden, which is the
	 * safe role: no watchdog, no attempt counter, and able to accept an update.
	 * A register the BootROM is free to leave at any value must fail that way.
	 */
	if (slot != UPDATE_SLOT_INDEX) {
		/* ---- GOLDEN ---------------------------------------------------- */
		ARB("golden (slot %u) present=%u attempts=%u", (unsigned)slot,
		    (unsigned)st.update_present, (unsigned)st.boot_attempts);

		/*
		 * Slot 0 is the ordinary way to be golden. Any other value means the
		 * BootROM went looking and landed here, which tells the redirect below
		 * that the armed image did not load.
		 *
		 * COUNT IT AS AN ATTEMPT, OR THE BOARD LOOPS. The increment further down
		 * lives in the UPDATE branch, which a wrapped boot never reaches, so
		 * leaving the counter alone means redirecting to the same unbootable
		 * image forever.
		 */
		if (slot != 0U) {
			ARB("arrived via BootROM search, not a redirect - the armed image "
			    "at slot %u did not load", (unsigned)UPDATE_SLOT_INDEX);
			st.boot_attempts++;
			br = boot_state_write(&st);
			if (br != BS_OK) {
				/* Unlike the increment in the UPDATE branch, this one cannot be
				 * shrugged off: without it the redirect loops. If the record will
				 * not take the count, refuse to redirect and stay in golden. */
				ARB("attempt count not persisted (%s) - staying golden",
				    bs_result_str(br));
				qspi_ctrl_restore(&save);
				return XST_SUCCESS;
			}
		}

		if (st.update_present && st.boot_attempts < BOOT_ATTEMPTS_MAX) {
			ARB("valid update - redirecting to 0x%08X", (unsigned)UPDATE_BASE);
			/* Put the controller back BEFORE the reset: multiboot_to() does not
			 * return on success, so there is no path back to the restore at the
			 * bottom of this function. */
			qspi_ctrl_restore(&save);
			(void)multiboot_to(UPDATE_BASE);
			/* Only reached if the offset was refused — nothing was written and
			 * nothing was reset, so carry on booting golden. */
			ARB("redirect refused - staying golden");
			return XST_SUCCESS;
		}

		qspi_ctrl_restore(&save);
		return XST_SUCCESS;
	}

	/* ---- UPDATE -------------------------------------------------------- */
	ARB("update (slot %u) attempts=%u", (unsigned)slot,
	    (unsigned)st.boot_attempts);

	if (st.boot_attempts >= BOOT_ATTEMPTS_MAX) {
		ARB("attempts exhausted - falling back to golden");
		qspi_ctrl_restore(&save);
		(void)multiboot_to(GOLDEN_BASE);
		ARB("fallback refused - continuing in update");
		return XST_SUCCESS;
	}

	/*
	 * Count this attempt before the application gets a chance to hang. The
	 * application clears it on commit; if it never gets that far, the next boot
	 * sees a higher number and eventually stops trying.
	 *
	 * A write failure here is tolerated: it costs one attempt of fallback
	 * margin, far better than signalling failure into FsblFallback() and
	 * corrupting the slot register.
	 */
	st.boot_attempts++;
	br = boot_state_write(&st);
	if (br != BS_OK) {
		ARB("attempt count not persisted (%s)", bs_result_str(br));
	}

	qspi_ctrl_restore(&save);
	return XST_SUCCESS;
}

u32 FsblHookBeforeHandoff(void)
{
	return XST_SUCCESS;
}

u32 FsblHookAfterBitstreamDload(void)
{
	return XST_SUCCESS;
}

void FsblHookFallback(void)
{
	/*
	 * MUST NOT RETURN. main.c calls this as a terminal action from paths that
	 * cannot recover — at main.c:232 after PS7_INIT_FAIL, where the devcfg
	 * driver is not yet initialised so a normal Fallback() is impossible.
	 * Returning would let the FSBL run on through code that assumes it never
	 * got here.
	 *
	 * No arbitration is attempted: this path is reached when the FSBL has
	 * already decided it cannot boot what it has. A power cycle clears
	 * MULTIBOOT_ADDR and lands back in golden regardless.
	 */
	xil_printf("ARB: FSBL fallback - halted, power-cycle to return to golden\r\n");
	while (1) {
		/* deliberately forever */
	}
}
