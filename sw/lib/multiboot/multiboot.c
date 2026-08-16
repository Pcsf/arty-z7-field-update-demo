/*
 * multiboot.c — Zynq-7000 MultiBoot redirect.
 *
 * See multiboot.h for the contract and for where every constant below was
 * traced to.
 */

#include "xil_io.h"
#include "xparameters.h"
#include "xuartps_hw.h"

#include "multiboot.h"

/* xparameters_ps.h:161 / :168 */
#define SLCR_BASE            0xF8000000U
#define DEVCFG_BASE          0xF8007000U

/* fsbl.h:475-476 — the vendor writes the key doubled into both halves; the
 * register only latches the low 16 bits, so 0xDF0D alone also works. */
#define SLCR_LOCK_OFFSET     0x004U
#define SLCR_UNLOCK_OFFSET   0x008U
#define SLCR_LOCKSTA_OFFSET  0x00CU
#define SLCR_LOCK_KEY        0x767B767BU
#define SLCR_UNLOCK_KEY      0xDF0DDF0DU

/* SLCR+0x200. The one constant without an in-tree oracle — see multiboot.h.
 * Bit 0 is the PS software reset (PS_RST_MASK, fsbl.h:408). */
#define SLCR_PSS_RST_CTRL    0x200U
#define PSS_RST_SOFT         0x1U

/* xdevcfg_hw.h:62 */
#define DEVCFG_MULTIBOOT     0x02CU

static u32
slcr_read(u32 off)
{
	return Xil_In32(SLCR_BASE + off);
}

static void
slcr_write(u32 off, u32 val)
{
	Xil_Out32(SLCR_BASE + off, val);
}

u32
multiboot_read_raw(void)
{
	return Xil_In32(DEVCFG_BASE + DEVCFG_MULTIBOOT);
}

u32
multiboot_slot_index(void)
{
	return multiboot_read_raw() & MB_ADDR_MASK;
}

u32
multiboot_current_offset(void)
{
	return multiboot_slot_index() * MB_SLOT_UNIT;
}

int
multiboot_slcr_is_locked(void)
{
	/* LOCKSTA bit 0: 1 = writes are being ignored. */
	return (slcr_read(SLCR_LOCKSTA_OFFSET) & 0x1U) != 0U;
}

void
multiboot_slcr_unlock(void)
{
	slcr_write(SLCR_UNLOCK_OFFSET, SLCR_UNLOCK_KEY);
}

void
multiboot_slcr_lock(void)
{
	slcr_write(SLCR_LOCK_OFFSET, SLCR_LOCK_KEY);
}

mb_result_t
multiboot_arm(u32 flash_offset)
{
	u32 slot;
	u32 reg;

	if ((flash_offset % MB_SLOT_UNIT) != 0U) {
		return MB_ERR_ALIGN;
	}

	slot = flash_offset / MB_SLOT_UNIT;

	/* Refuse rather than truncate: a truncated large offset very often aliases
	 * onto 0 — the golden image — which looks like the redirect silently not
	 * working rather than like a bug here. */
	if (slot > MB_ADDR_MASK) {
		return MB_ERR_RANGE;
	}

	/* Preserve everything above the address field; those bits are reserved
	 * rather than known-zero, and the FSBL treats the register the same way. */
	reg = multiboot_read_raw();
	reg = (reg & ~(u32)MB_ADDR_MASK) | slot;

	Xil_Out32(DEVCFG_BASE + DEVCFG_MULTIBOOT, reg);

	return MB_OK;
}

/*
 * Spin until the UART transmitter is fully idle, or until the ceiling is hit.
 *
 * IDLE IS TWO BITS. TXEMPTY says the transmit FIFO is empty; it does NOT say the
 * byte that just left the FIFO has finished going out. The shift register is a
 * separate stage and TACTIVE reports it, so waiting on TXEMPTY alone returns
 * with one character still on the wire — which the soft reset then destroys.
 * Both bits come from ONE read: two Xil_In32 calls could straddle the moment the
 * transmitter goes idle and see a mixture that never existed.
 *
 * Calibration: one iteration is ~113 ns, so MB_DRAIN_SPINS is ~225 ms. A full
 * 64-byte FIFO clears in 5.55 ms at 115200 8N1 and waiting for TACTIVE adds one
 * character time, ~87 us, so the bound is nowhere near binding. It is bounded at
 * all because this runs inside the arbiter: a UART that never goes idle must
 * cost a bounded delay, never a boot.
 *
 * Iterations rather than usleep(): this links into the FSBL as well as the
 * application, and the FSBL runs before the timers usleep() depends on are set
 * up. A counted loop cannot be wrong about a clock it has not configured yet.
 *
 * The status register is read directly because in this BSP
 * XUartPs_IsTransmitEmpty() is a FUNCTION in the xuartps driver, not a macro.
 * Calling it links cleanly in the application and breaks every app that links
 * this module without that driver:
 *
 *     multiboot.o: in function `multiboot_console_drain':
 *     undefined reference to `XUartPs_IsTransmitEmpty'
 *
 * The offsets and mask still come from the vendor header, so there is no second
 * source of truth for either.
 */
#define MB_DRAIN_SPINS  2000000U

/*
 * Returns the iterations consumed: a value below the ceiling means idle was
 * observed, the ceiling itself means it never was.
 */
static u32
multiboot_console_drain(void)
{
	u32 i;
	u32 sr;

	for (i = 0U; i < MB_DRAIN_SPINS; i++) {
		sr = Xil_In32(STDOUT_BASEADDRESS + XUARTPS_SR_OFFSET);

		if (((sr & XUARTPS_SR_TXEMPTY) != 0U) &&
		    ((sr & XUARTPS_SR_TACTIVE) == 0U)) {
			break;
		}
	}

	return i;
}

mb_result_t
multiboot_to(u32 flash_offset)
{
	mb_result_t r = multiboot_arm(flash_offset);

	if (r != MB_OK) {
		/* Nothing has been written; the board is exactly as it was. */
		return r;
	}

	/* Let the console finish before the reset takes the UART with it. Here
	 * rather than at the call sites because every caller has this problem and
	 * only this function knows a reset is imminent. */
	(void)multiboot_console_drain();

	/* Not ceremony: with SLCR locked the write below is silently discarded and
	 * the board simply carries on. */
	multiboot_slcr_unlock();

	Xil_Out32(SLCR_BASE + SLCR_PSS_RST_CTRL, PSS_RST_SOFT);

	/* The reset takes effect in its own time. Falling out of here while it
	 * lands would run the call site with the PS half-reset. */
	for (;;) {
		;
	}

	/* not reached */
}

const char *
mb_result_str(mb_result_t r)
{
	switch (r) {
	case MB_OK:         return "ok";
	case MB_ERR_ALIGN:  return "offset is not a multiple of 32 KiB";
	case MB_ERR_RANGE:  return "offset needs more than 13 bits";
	default:            return "unknown";
	}
}
