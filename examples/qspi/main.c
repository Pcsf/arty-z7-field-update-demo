/*
 * main.c — QSPI flash driver exercise.
 *
 * Writes non-volatile memory, so a bug here outlives the run that caused it.
 * That is why the write guard in qspi_flash.c exists before the first erase
 * rather than after the first accident.
 *
 * THE SEQUENCE, AND WHY IT IS IN THIS ORDER
 *
 *   1. JEDEC ID        does the bus work at all, and what part is this?
 *   2. read            can we read the sector we are about to destroy?
 *   3. erase           the destructive step, on a scratch sector only
 *   4. verify erased   did the erase actually take, or just return OK?
 *   5. program         512 bytes ACROSS a page boundary — see below
 *   6. read back       byte-for-byte, reporting the first mismatch offset
 *   7. guard tests     prove offset 0 is refused, prove the ceiling is refused
 *
 * Steps 1 and 2 come first because they are non-destructive and they are what
 * distinguishes "this part is not responding" from "this erase failed". An app
 * that erased first would report a failure at step 4 whether the flash was
 * broken or merely absent.
 *
 * Step 4 is not redundant with step 6. Erase and program fail in different
 * ways: a sector that never erased still reads back the pattern correctly for
 * every bit the pattern wanted cleared, because programming only clears bits.
 * Testing them together lets a completely dead erase pass on the right data.
 *
 * WHY 512 BYTES AND NOT 256
 *
 * The page-program command wraps within its 256-byte page. Programming 512
 * bytes as one command therefore overwrites the first half with the second and
 * leaves the following page erased — and the command reports success. A test
 * that only ever wrote one page would never see it, and the updater writes
 * megabytes.
 *
 * The pattern makes the wrap loud rather than merely detectable: page 0 is an
 * ascending ramp and page 1 is its complement. A wrapped write leaves the
 * complement sitting in page 0 and 0xFF in page 1, so the mismatch appears at
 * offset 0 instead of somewhere subtle.
 *
 * ON VERIFICATION
 *
 * Console only, deliberately. The register app mirrors its results into DDR for
 * inspection over JTAG, which was the right call when the serial console was
 * broken. It works now, and ad-hoc `mrd` sessions on this board correlate with
 * a wedged DAP and a physical replug of J14 — so the cheap habit is the one
 * that does not cost a cable.
 */

#include <string.h>

#include "xil_printf.h"

#include "qspi_flash.h"

/* 512 bytes = two pages, which is the whole point. See the header comment. */
#define PATTERN_LEN     (2U * QSPI_PAGE_SIZE)

/* How much of the erased sector to check. One page is enough to catch an erase
 * that did not happen; checking the whole 64 KB would add seconds of console
 * time to prove the same thing. */
#define ERASE_CHECK_LEN QSPI_PAGE_SIZE

static u8 pattern[PATTERN_LEN];
static u8 readback[PATTERN_LEN];

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

static void
hexdump(const u8 *p, u32 len)
{
	u32 i;

	xil_printf("    ");
	for (i = 0U; i < len; i++) {
		xil_printf("%02x ", p[i]);
	}
	xil_printf("\r\n");
}

/*
 * Compare and report the FIRST difference with both values.
 *
 * "Read-back mismatch" tells you nothing. The offset tells you which failure
 * you have: a mismatch at 0 with the complement pattern present is a page-wrap;
 * a mismatch at 256 with 0xFF is a program that stopped after one page; 0xFF
 * everywhere is a program that never happened; scattered single bits are a
 * clocking or signal-integrity problem. One number separates four causes.
 */
static int
compare(const u8 *want, const u8 *got, u32 len)
{
	u32 i;

	for (i = 0U; i < len; i++) {
		if (want[i] != got[i]) {
			xil_printf("    first mismatch at offset %d: wrote 0x%02x, read 0x%02x\r\n",
			           (int)i, want[i], got[i]);
			return 0;
		}
	}
	return 1;
}

static int
all_erased(const u8 *p, u32 len)
{
	u32 i;

	for (i = 0U; i < len; i++) {
		if (p[i] != 0xFFU) {
			xil_printf("    not erased at offset %d: 0x%02x\r\n", (int)i, p[i]);
			return 0;
		}
	}
	return 1;
}

int
main(void)
{
	qspi_result_t r;
	u8            mfr = 0U, type = 0U, capacity = 0U;
	u32           bytes;
	u32           i;

	xil_printf("\r\n--- QSPI flash ---\r\n");
	xil_printf("Scratch sector : 0x%08x\r\n", QSPI_SCRATCH_ADDR);
	xil_printf("Writable window: 0x%08x .. 0x%08x\r\n",
	           QSPI_WRITE_FLOOR, QSPI_WRITE_CEIL);

	r = qspi_flash_init();
	if (r != QSPI_OK) {
		xil_printf("FATAL: init: %s (code %d)\r\n", qspi_result_str(r), (int)r);
		goto done;
	}
	xil_printf("Controller     : up\r\n\r\n");

	/* ---- 1. identify -------------------------------------------------- */

	xil_printf("1. JEDEC ID\r\n");
	r = qspi_flash_read_id(&mfr, &type, &capacity);
	if (r != QSPI_OK) {
		xil_printf("  read_id: %s (code %d)\r\n", qspi_result_str(r), (int)r);
		check("JEDEC ID read", 0);
		goto done;
	}

	xil_printf("    manufacturer 0x%02x  type 0x%02x  capacity 0x%02x\r\n",
	           mfr, type, capacity);

	/* 0x00 and 0xFF are what a bus with nothing on it reads. Neither is a
	 * manufacturer code, so either one means the pins, not the part. */
	check("manufacturer byte is not 0x00/0xFF",
	      (mfr != 0x00U) && (mfr != 0xFFU));

	bytes = qspi_flash_capacity_bytes(capacity);
	xil_printf("    decoded capacity: %d MiB\r\n", (int)(bytes / (1024U * 1024U)));
	check("capacity decodes to 16 MiB", bytes == QSPI_FLASH_SIZE);

	/* ---- 2. read before destroying ------------------------------------ */

	xil_printf("\r\n2. Read scratch sector (before erase)\r\n");
	r = qspi_flash_read(QSPI_SCRATCH_ADDR, readback, 16U);
	if (r == QSPI_OK) {
		hexdump(readback, 16U);
	} else {
		xil_printf("    read: %s (code %d)\r\n", qspi_result_str(r), (int)r);
	}
	check("read scratch sector", r == QSPI_OK);

	/* ---- 3. erase ------------------------------------------------------ */

	xil_printf("\r\n3. Erase scratch sector\r\n");
	r = qspi_flash_erase_sector(QSPI_SCRATCH_ADDR);
	if (r != QSPI_OK) {
		xil_printf("    erase: %s (code %d)\r\n", qspi_result_str(r), (int)r);
	}
	check("erase sector", r == QSPI_OK);

	/* ---- 4. verify the erase actually happened ------------------------- */

	xil_printf("\r\n4. Verify erased (first %d bytes)\r\n", (int)ERASE_CHECK_LEN);
	r = qspi_flash_read(QSPI_SCRATCH_ADDR, readback, ERASE_CHECK_LEN);
	if (r != QSPI_OK) {
		xil_printf("    read: %s (code %d)\r\n", qspi_result_str(r), (int)r);
		check("read after erase", 0);
	} else {
		hexdump(readback, 16U);
		check("all bytes are 0xFF after erase",
		      all_erased(readback, ERASE_CHECK_LEN));
	}

	/* ---- 5. program across a page boundary ----------------------------- */

	for (i = 0U; i < PATTERN_LEN; i++) {
		u8 ramp = (u8)(i & 0xFFU);

		/* Page 0 ascending, page 1 its complement — a page-wrap then shows up
		 * as the complement at offset 0, not as a subtle shift. */
		pattern[i] = (i < QSPI_PAGE_SIZE) ? ramp : (u8)(~ramp);
	}

	xil_printf("\r\n5. Program %d bytes across the page boundary\r\n",
	           (int)PATTERN_LEN);
	r = qspi_flash_program(QSPI_SCRATCH_ADDR, pattern, PATTERN_LEN);
	if (r != QSPI_OK) {
		xil_printf("    program: %s (code %d)\r\n", qspi_result_str(r), (int)r);
	}
	check("program 512 bytes", r == QSPI_OK);

	/* ---- 6. read back and compare -------------------------------------- */

	xil_printf("\r\n6. Read back and compare\r\n");
	memset(readback, 0x00, sizeof(readback));

	r = qspi_flash_read(QSPI_SCRATCH_ADDR, readback, PATTERN_LEN);
	if (r != QSPI_OK) {
		xil_printf("    read: %s (code %d)\r\n", qspi_result_str(r), (int)r);
		check("read back 512 bytes", 0);
	} else {
		xil_printf("    page 0 head:\r\n");
		hexdump(readback, 16U);
		xil_printf("    page 1 head:\r\n");
		hexdump(&readback[QSPI_PAGE_SIZE], 16U);
		check("read back matches written pattern, 512/512",
		      compare(pattern, readback, PATTERN_LEN));
	}

	/* ---- 7. the guard -------------------------------------------------- */

	/*
	 * The most important checks in this app, and the cheapest.
	 *
	 * These call the real erase and program entry points with real addresses
	 * that must be refused. They are safe by construction: the guard runs
	 * before any command reaches the controller, so a passing test issues
	 * nothing to the part. If the guard were broken, the first of these would
	 * erase the golden sector — which is why it is worth testing while that
	 * sector is still empty.
	 */
	xil_printf("\r\n7. Write guard\r\n");

	r = qspi_flash_erase_sector(0x00000000U);
	xil_printf("    erase(0x00000000) -> %s (code %d)\r\n",
	           qspi_result_str(r), (int)r);
	check("erase at offset 0 is refused", r == QSPI_ERR_RANGE);

	r = qspi_flash_erase_sector(QSPI_WRITE_FLOOR - QSPI_SECTOR_SIZE);
	xil_printf("    erase(0x%08x) -> %s (code %d)\r\n",
	           QSPI_WRITE_FLOOR - QSPI_SECTOR_SIZE, qspi_result_str(r), (int)r);
	check("erase just below the floor is refused", r == QSPI_ERR_RANGE);

	r = qspi_flash_erase_sector(QSPI_WRITE_CEIL);
	xil_printf("    erase(0x%08x) -> %s (code %d)\r\n",
	           QSPI_WRITE_CEIL, qspi_result_str(r), (int)r);
	check("erase of the state sector is refused", r == QSPI_ERR_RANGE);

	/* A program that starts legally and runs off the end of the window. The
	 * span check, not the start check, is what catches this. */
	r = qspi_flash_program(QSPI_WRITE_CEIL - 16U, pattern, 32U);
	xil_printf("    program(ceil-16, 32) -> %s (code %d)\r\n",
	           qspi_result_str(r), (int)r);
	check("program spanning the ceiling is refused", r == QSPI_ERR_RANGE);

	/* An unaligned erase is a caller bug and must not be silently rounded. */
	r = qspi_flash_erase_sector(QSPI_SCRATCH_ADDR + 4U);
	xil_printf("    erase(scratch+4) -> %s (code %d)\r\n",
	           qspi_result_str(r), (int)r);
	check("unaligned erase is refused", r == QSPI_ERR_ARG);

done:
	xil_printf("\r\n%d / %d checks passed\r\n",
	           (int)(checks_run - checks_failed), (int)checks_run);

	if (checks_failed == 0U && checks_run > 0U) {
		xil_printf("--- QSPI: ALL CHECKS PASSED ---\r\n");
	} else {
		xil_printf("--- QSPI: FAIL ---\r\n");
	}

	/* Idle rather than return — returning from main() on bare metal parks the
	 * core in the C runtime's exit loop with the MMU still on, which is a
	 * confusing place to find it when you attach with xsct. */
	while (1) {
		;
	}

	return 0;
}
