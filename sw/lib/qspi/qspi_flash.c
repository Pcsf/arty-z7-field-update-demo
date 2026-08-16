/*
 * QSPI serial-NOR flash driver.
 *
 * Sits on the BSP's XQspiPs driver (qspips_v3_9) in manual-start IO mode. See
 * qspi_flash.h for the contract, the part-agnostic command argument, and the
 * write-guard rationale; this file is the mechanics.
 *
 * WHY MANUAL START AND NOT LINEAR MODE
 *
 * The Zynq QSPI controller also has LQSPI — a linear, memory-mapped read window
 * at 0xFC000000 that makes the flash look like ROM. It is genuinely nicer for
 * reading, and it is useless here: it is a read path only. It cannot issue WREN,
 * SE or PP, so a driver that used it would still need this one for every write,
 * and the read-back verification would then be reading through a *different*
 * path than the one that did the writing. Verifying a program through a second
 * mechanism that shares no code with the first sounds like a strength and is
 * actually a way to be fooled by a controller-level cache. One path, used for
 * both directions.
 *
 * ON XQspiPs_PolledTransfer AND FULL DUPLEX
 *
 * SPI is symmetric: every byte clocked out clocks one in. The driver reflects
 * that — one ByteCount covers command, address and data together, and the
 * receive buffer is filled from byte 0. So for a read of N bytes at a 3-byte
 * address the transfer is 4+N bytes long, the first four received bytes are the
 * garbage clocked in while the command and address went out, and the payload
 * starts at rx[4]. Getting this offset wrong is the classic way to produce a
 * dump that looks almost right and is shifted by four.
 *
 * The send buffer must be the full ByteCount even when only the first bytes
 * matter — whatever follows is clocked out as don't-care while the part is
 * driving. Ours is a static buffer, so it is deterministic don't-care.
 */

#include <string.h>

#include "xparameters.h"
#include "xqspips.h"
#include "xstatus.h"
#include "sleep.h"

#include "qspi_flash.h"

/* JEDEC-standard command opcodes — see the header on why only these. */
#define CMD_RDID        0x9FU
#define CMD_READ        0x03U
#define CMD_PP          0x02U
#define CMD_SE          0xD8U
#define CMD_WREN        0x06U
#define CMD_RDSR        0x05U

#define SR_WIP          0x01U   /* status register bit 0: write in progress */
#define SR_WEL          0x02U   /* status register bit 1: write enable latch */

/* Command byte plus three address bytes precede every payload. */
#define CMD_ADDR_LEN    4U

/*
 * WIP poll budget. A 64 KB sector erase is typically a few hundred
 * milliseconds and specified in the seconds on most parts, so ten seconds is
 * "the part is not coming back" rather than "the part is slow". A page program
 * finishes in well under a millisecond and exits on the first poll.
 */
#define WIP_POLL_MAX    10000U  /* iterations */
#define WIP_POLL_US     1000U   /* microseconds between polls */

/*
 * 25 MHz: 200 MHz QSPI reference (XPAR_XQSPIPS_0_QSPI_CLK_FREQ_HZ) over the
 * prescaler's divide-by-8. Every part in this class does 50 MHz or better for
 * the commands used here, so this is not near any limit — deliberately. The
 * update path is bounded by a 100BASE-TX TFTP fetch, not by flash clocking, so
 * there is nothing to buy by running the bus fast and signal integrity to lose.
 */
#define QSPI_PRESCALER  XQSPIPS_CLK_PRESCALE_8

static XQspiPs qspi;
static int     qspi_ready;

/*
 * One transfer buffer pair, sized for the largest single transaction the driver
 * issues: a full page program (command + address + 256 bytes). Reads are
 * chunked to the same size so there is one buffer discipline rather than two.
 */
static u8 tx_buf[CMD_ADDR_LEN + QSPI_PAGE_SIZE];
static u8 rx_buf[CMD_ADDR_LEN + QSPI_PAGE_SIZE];

/* Pack a 24-bit address MSB-first behind the command byte. */
static void
put_cmd_addr(u8 cmd, u32 addr)
{
	tx_buf[0] = cmd;
	tx_buf[1] = (u8)((addr >> 16) & 0xFFU);
	tx_buf[2] = (u8)((addr >> 8) & 0xFFU);
	tx_buf[3] = (u8)(addr & 0xFFU);
}

/*
 * PASSING A RECEIVE BUFFER TURNS THE TRANSFER INTO A READ. THIS IS NOT
 * OPTIONAL BOOKKEEPING — IT CHANGES WHAT GOES OUT ON THE WIRE.
 *
 * The obvious reading of XQspiPs_PolledTransfer(inst, tx, rx, n) is that `rx`
 * is an output parameter: supply one and you also get the bytes that were
 * clocked in, supply NULL and you don't. That is wrong, and the driver says so
 * only in its source (xqspips.c, the FIFO fill loop):
 *
 *     if (InstancePtr->RecvBufferPtr &&
 *         ((InstancePtr->RequestedBytes - InstancePtr->RemainingBytes) > 4)) {
 *             ... write XQSPIPS_DUMMY_TX_DATA ...
 *     } else {
 *             ... write the caller's data ...
 *     }
 *
 * A non-NULL RecvBufferPtr means "this is a flash read", so after the first
 * four bytes — the command and its 3-byte address — the driver transmits DUMMY
 * instead of the send buffer, because on a read those cycles exist only to
 * clock data back in.
 *
 * For a page program that is silent corruption. The command and address go out
 * correctly, the first four-byte FIFO write still carries real data (that chunk
 * fails the `> 4` test), and every byte after it is a dummy. The part programs
 * exactly four correct bytes per page, reports no error, and the driver returns
 * XST_SUCCESS — the flash reads back as `00 01 02 03 FF FF …`.
 *
 * So writes pass NULL and reads pass rx_buf, and the two are separate functions
 * rather than an argument, because this is exactly the distinction a future
 * edit would collapse "for symmetry".
 */
static qspi_result_t
transfer_read(u32 len)
{
	if (XQspiPs_PolledTransfer(&qspi, tx_buf, rx_buf, len) != XST_SUCCESS) {
		return QSPI_ERR_XFER;
	}
	return QSPI_OK;
}

static qspi_result_t
transfer_write(u32 len)
{
	if (XQspiPs_PolledTransfer(&qspi, tx_buf, NULL, len) != XST_SUCCESS) {
		return QSPI_ERR_XFER;
	}
	return QSPI_OK;
}

/* Read the status register. */
static qspi_result_t
read_status(u8 *sr)
{
	qspi_result_t r;

	tx_buf[0] = CMD_RDSR;
	tx_buf[1] = 0U;

	r = transfer_read(2U);
	if (r != QSPI_OK) {
		return r;
	}

	*sr = rx_buf[1];
	return QSPI_OK;
}

/*
 * Set the write-enable latch. Every erase and every program needs this
 * immediately before it — the latch is cleared by the part on completion of the
 * operation, so it is one WREN per command and never one per batch.
 */
static qspi_result_t
write_enable(void)
{
	tx_buf[0] = CMD_WREN;
	return transfer_write(1U);
}

/*
 * Block until the part reports the write finished.
 *
 * This is the only place that can hang, so it is the only place with a timeout.
 * Returning QSPI_ERR_TIMEOUT rather than spinning forever matters: an updater
 * that hangs here never services its watchdog, and the failure presents as a
 * mysterious reset loop rather than as a flash fault.
 */
static qspi_result_t
wait_while_busy(void)
{
	u32 i;

	for (i = 0U; i < WIP_POLL_MAX; i++) {
		u8            sr = 0U;
		qspi_result_t r  = read_status(&sr);

		if (r != QSPI_OK) {
			return r;
		}
		if ((sr & SR_WIP) == 0U) {
			return QSPI_OK;
		}
		usleep(WIP_POLL_US);
	}

	return QSPI_ERR_TIMEOUT;
}

/*
 * THE GUARD.
 *
 * Called first by both write entry points, before any WREN and before anything
 * reaches the controller — so a rejected call leaves the part in exactly the
 * state it was in, having been sent nothing at all.
 *
 * The arithmetic is done so it cannot wrap: `len` is bounded against the flash
 * size before addr+len is formed. An overflowing end address that lands back
 * inside the window is precisely the bug this function exists to not have.
 */
static qspi_result_t
guard_write_range(u32 addr, u32 len)
{
	u32 end;

	if (len == 0U || len > QSPI_FLASH_SIZE) {
		return QSPI_ERR_ARG;
	}
	if (addr > QSPI_FLASH_SIZE - len) {
		return QSPI_ERR_RANGE;
	}

	end = addr + len;

	if (addr < QSPI_WRITE_FLOOR || end > QSPI_WRITE_CEIL) {
		return QSPI_ERR_RANGE;
	}

	return QSPI_OK;
}

qspi_result_t
qspi_flash_init(void)
{
	XQspiPs_Config *cfg;

	if (qspi_ready) {
		return QSPI_OK;
	}

	cfg = XQspiPs_LookupConfig(XPAR_XQSPIPS_0_DEVICE_ID);
	if (cfg == NULL) {
		return QSPI_ERR_INIT;
	}

	if (XQspiPs_CfgInitialize(&qspi, cfg, cfg->BaseAddress) != XST_SUCCESS) {
		return QSPI_ERR_INIT;
	}

	/*
	 * Three options, all load-bearing on Zynq:
	 *
	 *   MANUAL_START   the driver starts each transfer explicitly rather than
	 *                  auto-starting when the FIFO fills, which is what makes a
	 *                  command+address+data sequence one atomic transaction.
	 *   FORCE_SSELECT  hold chip-select asserted across that whole transaction.
	 *                  Without it the part sees the command and the data as
	 *                  separate transactions and ignores the second.
	 *   HOLD_B_DRIVE   drive the HOLD_B/IO3 pin rather than leaving it floating.
	 *                  The board wires it to MIO for x4, and a floating HOLD_B
	 *                  can hold the part mid-transfer — an intermittent failure
	 *                  that looks like a bad part.
	 */
	if (XQspiPs_SetOptions(&qspi,
	                       XQSPIPS_MANUAL_START_OPTION |
	                       XQSPIPS_FORCE_SSELECT_OPTION |
	                       XQSPIPS_HOLD_B_DRIVE_OPTION) != XST_SUCCESS) {
		return QSPI_ERR_INIT;
	}

	if (XQspiPs_SetClkPrescaler(&qspi, QSPI_PRESCALER) != XST_SUCCESS) {
		return QSPI_ERR_INIT;
	}

	if (XQspiPs_SetSlaveSelect(&qspi) != XST_SUCCESS) {
		return QSPI_ERR_INIT;
	}

	qspi_ready = 1;
	return QSPI_OK;
}

qspi_result_t
qspi_flash_force_reinit(void)
{
	/*
	 * qspi_flash_init() is idempotent by design, which is right for
	 * an application that owns the controller for its whole life and wrong for
	 * the FSBL arbiter, which borrows a controller somebody else has configured
	 * for linear reads. If the arbiter ran twice in one boot, the second call
	 * would see qspi_ready and skip re-applying manual mode -- and manual-mode
	 * transfers against a controller left in linear mode misbehave quietly
	 * rather than failing, which is the worst available outcome.
	 *
	 * So: drop the cached state and configure from scratch, every time.
	 */
	qspi_ready = 0;
	return qspi_flash_init();
}

qspi_result_t
qspi_flash_read_id(u8 *mfr, u8 *type, u8 *capacity)
{
	qspi_result_t r;

	if (!qspi_ready) {
		return QSPI_ERR_STATE;
	}

	tx_buf[0] = CMD_RDID;
	memset(&tx_buf[1], 0, 3);

	r = transfer_read(4U);
	if (r != QSPI_OK) {
		return r;
	}

	/* rx[0] is the byte clocked in while the command went out. */
	if (mfr != NULL) {
		*mfr = rx_buf[1];
	}
	if (type != NULL) {
		*type = rx_buf[2];
	}
	if (capacity != NULL) {
		*capacity = rx_buf[3];
	}

	return QSPI_OK;
}

u32
qspi_flash_capacity_bytes(u8 capacity_byte)
{
	/*
	 * JEDEC encodes capacity as a power of two. Bounded rather than shifted
	 * blindly: a dead bus reads 0x00 or 0xFF, and 1U << 0xFF is undefined
	 * behaviour, not a large number. The range covers 64 KiB to 256 MiB, which
	 * brackets every part that could plausibly be on a board like this.
	 */
	if ((u32)capacity_byte < 0x10U || (u32)capacity_byte > 0x1CU) {
		return 0U;
	}
	return 1U << (u32)capacity_byte;
}

qspi_result_t
qspi_flash_read(u32 addr, u8 *dst, u32 len)
{
	u32 done = 0U;

	if (!qspi_ready) {
		return QSPI_ERR_STATE;
	}
	if (dst == NULL || len == 0U) {
		return QSPI_ERR_ARG;
	}
	if (len > QSPI_FLASH_SIZE || addr > QSPI_FLASH_SIZE - len) {
		return QSPI_ERR_ARG;
	}

	/*
	 * Chunked to the transfer buffer. READ itself streams across page and
	 * sector boundaries without help — the chunking is about buffer size, not
	 * about the part, which is why the chunk boundary is arbitrary here and
	 * load-bearing in qspi_flash_program().
	 */
	while (done < len) {
		u32           n = len - done;
		qspi_result_t r;

		if (n > QSPI_PAGE_SIZE) {
			n = QSPI_PAGE_SIZE;
		}

		put_cmd_addr(CMD_READ, addr + done);
		memset(&tx_buf[CMD_ADDR_LEN], 0, n);

		r = transfer_read(CMD_ADDR_LEN + n);
		if (r != QSPI_OK) {
			return r;
		}

		memcpy(&dst[done], &rx_buf[CMD_ADDR_LEN], n);
		done += n;
	}

	return QSPI_OK;
}

qspi_result_t
qspi_flash_erase_sector(u32 addr)
{
	qspi_result_t r;

	if (!qspi_ready) {
		return QSPI_ERR_STATE;
	}

	/* Alignment before range: an unaligned address is a caller bug regardless
	 * of where it points, and reporting it as a range error would send whoever
	 * is debugging to the wrong place. */
	if ((addr % QSPI_SECTOR_SIZE) != 0U) {
		return QSPI_ERR_ARG;
	}

	/* THE GUARD — before any command is issued. Nothing below this line runs
	 * for a rejected address. */
	r = guard_write_range(addr, QSPI_SECTOR_SIZE);
	if (r != QSPI_OK) {
		return r;
	}

	r = write_enable();
	if (r != QSPI_OK) {
		return r;
	}

	put_cmd_addr(CMD_SE, addr);
	r = transfer_write(CMD_ADDR_LEN);
	if (r != QSPI_OK) {
		return r;
	}

	return wait_while_busy();
}

qspi_result_t
qspi_flash_program(u32 addr, const u8 *src, u32 len)
{
	qspi_result_t r;
	u32           done = 0U;

	if (!qspi_ready) {
		return QSPI_ERR_STATE;
	}
	if (src == NULL) {
		return QSPI_ERR_ARG;
	}

	/* THE GUARD — before any command is issued, and over the whole span rather
	 * than per chunk, so a write that would start inside the window and run out
	 * of it is refused in full instead of half-completing. */
	r = guard_write_range(addr, len);
	if (r != QSPI_OK) {
		return r;
	}

	/*
	 * Split on page boundaries. The first chunk runs to the end of whatever
	 * page `addr` lands in — which is why this is not simply "256 bytes at a
	 * time": an unaligned start with fixed-size chunks would still cross a
	 * boundary on the first write, which is the exact fault being avoided.
	 */
	while (done < len) {
		u32 dst_addr = addr + done;
		u32 page_off = dst_addr % QSPI_PAGE_SIZE;
		u32 n        = QSPI_PAGE_SIZE - page_off;

		if (n > len - done) {
			n = len - done;
		}

		r = write_enable();
		if (r != QSPI_OK) {
			return r;
		}

		put_cmd_addr(CMD_PP, dst_addr);
		memcpy(&tx_buf[CMD_ADDR_LEN], &src[done], n);

		/* transfer_write, not transfer_read — see the long comment on those two
		 * functions. Passing rx_buf here is what corrupted the first silicon
		 * run, and it corrupts silently. */
		r = transfer_write(CMD_ADDR_LEN + n);
		if (r != QSPI_OK) {
			return r;
		}

		r = wait_while_busy();
		if (r != QSPI_OK) {
			return r;
		}

		done += n;
	}

	return QSPI_OK;
}

/*
 * ---- THE BOOT-STATE SECTOR ------------------------------------------------
 *
 * Separate entry points for the one sector the window above refuses, sharing the
 * low-level command layer rather than duplicating it: a second, unproven copy of
 * the SPI sequencing is the thing to avoid.
 *
 * None of these takes an absolute address. There is no argument to get wrong
 * that aims them at an image, so the separation from the update slot is
 * structural rather than range-checked. guard_write_range() is deliberately NOT
 * called here: it would reject every one of these by design.
 */

qspi_result_t
qspi_flash_state_erase(void)
{
	qspi_result_t r;

	if (!qspi_ready) {
		return QSPI_ERR_STATE;
	}

	r = write_enable();
	if (r != QSPI_OK) {
		return r;
	}

	put_cmd_addr(CMD_SE, QSPI_STATE_ADDR);
	r = transfer_write(CMD_ADDR_LEN);
	if (r != QSPI_OK) {
		return r;
	}

	return wait_while_busy();
}

qspi_result_t
qspi_flash_state_program(u32 sector_off, const u8 *src, u32 len)
{
	qspi_result_t r;
	u32           done = 0U;

	if (!qspi_ready) {
		return QSPI_ERR_STATE;
	}
	if (src == NULL || len == 0U) {
		return QSPI_ERR_ARG;
	}

	/* The only bound here: stay inside the one sector. Formed so it cannot
	 * wrap, for the same reason guard_write_range() is. */
	if (sector_off >= QSPI_SECTOR_SIZE ||
	    len > QSPI_SECTOR_SIZE - sector_off) {
		return QSPI_ERR_RANGE;
	}

	while (done < len) {
		u32 dst_addr = QSPI_STATE_ADDR + sector_off + done;
		u32 page_off = dst_addr % QSPI_PAGE_SIZE;
		u32 n        = QSPI_PAGE_SIZE - page_off;

		if (n > len - done) {
			n = len - done;
		}

		r = write_enable();
		if (r != QSPI_OK) {
			return r;
		}

		put_cmd_addr(CMD_PP, dst_addr);
		memcpy(&tx_buf[CMD_ADDR_LEN], &src[done], n);

		r = transfer_write(CMD_ADDR_LEN + n);
		if (r != QSPI_OK) {
			return r;
		}

		r = wait_while_busy();
		if (r != QSPI_OK) {
			return r;
		}

		done += n;
	}

	return QSPI_OK;
}

qspi_result_t
qspi_flash_state_read(u32 sector_off, u8 *dst, u32 len)
{
	if (sector_off >= QSPI_SECTOR_SIZE ||
	    len > QSPI_SECTOR_SIZE - sector_off) {
		return QSPI_ERR_RANGE;
	}

	/* Reads are unrestricted, so this delegates rather than reimplementing.
	 * The bound above keeps the caller honest about what a state-sector offset
	 * means; it is not protecting anything. */
	return qspi_flash_read(QSPI_STATE_ADDR + sector_off, dst, len);
}

const char *
qspi_result_str(qspi_result_t r)
{
	switch (r) {
	case QSPI_OK:          return "ok";
	case QSPI_ERR_ARG:     return "bad argument";
	case QSPI_ERR_INIT:    return "controller init failed";
	case QSPI_ERR_RANGE:   return "address outside writable window";
	case QSPI_ERR_XFER:    return "SPI transfer failed";
	case QSPI_ERR_TIMEOUT: return "timed out waiting for write";
	case QSPI_ERR_STATE:   return "qspi_flash_init() not called";
	default:               return "unknown";
	}
}
