/*
 * main.c — register access to blinkctl from the CPU.
 *
 * Reaches the peripheral the way the updater does: Xil_In32/Xil_Out32 from the
 * A9, out M_AXI_GP0, through the protocol converter, into blinkctl_axil.
 *
 * It proves three things a JTAG-to-AXI bench cannot:
 *   1. the PS can reach the fabric at all (GP0 is alive, addresses decode),
 *   2. the AXI3 -> AXI4-Lite conversion works for both reads and writes,
 *   3. FCLK_CLK0 runs at the rate project.mk asked for.
 *
 * ON MEASURING THE CLOCK
 *
 * HEARTBEAT increments once per FCLK_CLK0 cycle, so sampling it twice one
 * second apart yields the fabric clock in Hz. The reference is the CPU
 * timebase, which comes from the same PS PLLs, so this is a ratio check rather
 * than an absolute measurement — it cannot catch both clocks being wrong
 * together. It comfortably catches the case that matters: FCLK left at the
 * board preset's 100 MHz instead of the 125 MHz the ICD specifies, which no
 * amount of looking at the LED will tell you apart.
 *
 * ON NOT USING xparameters.h
 *
 * blinkctl lives outside the block design, so Vitis never sees it and emits no
 * XPAR_* symbols for it. The addresses come from doc/icd_blinkctl.md, which is
 * the source of truth for this register map and the same place the bench TC
 * list takes them from. Keep these in step with the ICD, not with the tools.
 *
 * ON THE DDR RESULT BLOCK
 *
 * Every value printed is also written to a fixed DDR address, so the run can be
 * verified over JTAG without a serial console:
 *
 *   xsct%  mrd -force 0x00800000 8
 *
 * That matters when /dev/ttyUSB* is unreadable — a result you can read back
 * from memory survives a missed console.
 */

#include "xil_io.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "sleep.h"

/* blinkctl — doc/icd_blinkctl.md. Base is GP0's window on Zynq-7000. */
#define BLINKCTL_BASE      0x43C00000U
#define REG_VERSION        0x00U
#define REG_CTRL           0x04U
#define REG_BLINK_DIV      0x08U
#define REG_HEARTBEAT      0x0CU
#define REG_SCRATCH        0x10U
#define REG_STATUS         0x14U

#define CTRL_BLINK_EN      0x1U
#define STATUS_DIVCLAMP    0x1U

/* Scratch pattern. Distinct from the bench's 0xCAFEF00D so a stale value is
 * obvious: if this reads back CAFEF00D, the CPU never wrote anything and you
 * are looking at what the JTAG session left behind. */
#define SCRATCH_PATTERN    0x5A5AC0DEU

/* Where results land for mrd. 8 MB into DDR, deliberately NOT 0x00100000:
 * standalone Zynq apps link at that address, so a result block there is
 * overwritten by the very application that wrote it. */
#define RESULT_BASE        0x00800000U
#define RESULT_MAGIC       0x50335247U   /* "P3RG" */

#define RD(off)      Xil_In32(BLINKCTL_BASE + (off))
#define WR(off, val) Xil_Out32(BLINKCTL_BASE + (off), (val))

int main(void)
{
    u32 version, blink_div, status;
    u32 scratch_before, scratch_after;
    u32 hb1, hb2, hb_delta;
    u32 failures = 0U;

    xil_printf("\r\n--- blinkctl register access ---\r\n");

    version   = RD(REG_VERSION);
    blink_div = RD(REG_BLINK_DIV);
    status    = RD(REG_STATUS);

    xil_printf("VERSION   = 0x%08x\r\n", version);
    xil_printf("BLINK_DIV = %u\r\n", blink_div);
    xil_printf("STATUS    = 0x%08x\r\n", status);

    /* VERSION's upper half is the major number; v1 and v2 are the only payloads
     * that exist, so anything else means we are not talking to blinkctl. */
    if ((version >> 16) != 1U && (version >> 16) != 2U) {
        xil_printf("FAIL: VERSION major is neither 1 nor 2\r\n");
        failures++;
    }

    /* SCRATCH round trip — the cheapest proof that writes reach the slave and
     * the write response comes back. A read-only bus would pass the reads
     * above and fail here. */
    scratch_before = RD(REG_SCRATCH);
    WR(REG_SCRATCH, SCRATCH_PATTERN);
    scratch_after = RD(REG_SCRATCH);
    xil_printf("SCRATCH   = 0x%08x -> 0x%08x\r\n", scratch_before, scratch_after);
    if (scratch_after != SCRATCH_PATTERN) {
        xil_printf("FAIL: SCRATCH did not hold the written value\r\n");
        failures++;
    }

    /* HEARTBEAT over one second of CPU time. usleep is driven by the PS
     * timebase; see the note at the top about what this does and does not
     * prove. Unsigned arithmetic makes the counter's wrap harmless. */
    hb1 = RD(REG_HEARTBEAT);
    usleep(1000000U);
    hb2 = RD(REG_HEARTBEAT);
    hb_delta = hb2 - hb1;

    xil_printf("HEARTBEAT = 0x%08x -> 0x%08x\r\n", hb1, hb2);
    xil_printf("FCLK_CLK0 ~ %u Hz\r\n", hb_delta);

    if (hb_delta == 0U) {
        xil_printf("FAIL: HEARTBEAT did not advance - fabric clock stopped?\r\n");
        failures++;
    } else if (hb_delta < 118000000U || hb_delta > 132000000U) {
        /* +/-5% around 125 MHz. A 100 MHz FCLK lands at ~100e6 and trips this,
         * which is the whole point of the check. */
        xil_printf("FAIL: expected ~125 MHz, measured %u Hz\r\n", hb_delta);
        failures++;
    }

    /* Leave the LED enabled regardless of how the run went, so the board is in
     * a sane state for whatever runs next. */
    WR(REG_CTRL, CTRL_BLINK_EN);

    /* Result block for 'mrd -force 0x00800000 8'. Magic first so a stale or
     * never-written buffer is distinguishable from a real result. */
    Xil_Out32(RESULT_BASE + 0x00U, RESULT_MAGIC);
    Xil_Out32(RESULT_BASE + 0x04U, version);
    Xil_Out32(RESULT_BASE + 0x08U, blink_div);
    Xil_Out32(RESULT_BASE + 0x0CU, scratch_after);
    Xil_Out32(RESULT_BASE + 0x10U, hb1);
    Xil_Out32(RESULT_BASE + 0x14U, hb2);
    Xil_Out32(RESULT_BASE + 0x18U, hb_delta);
    Xil_Out32(RESULT_BASE + 0x1CU, failures);

    /* The standalone BSP runs with the D-cache enabled, so those stores sit in
     * cache and DDR still holds whatever was there before. A JTAG 'mrd' reads
     * DDR directly and would see stale memory — the results must be flushed out
     * to be observable from the debugger at all. */
    Xil_DCacheFlushRange((INTPTR)RESULT_BASE, 32U);

    if (failures == 0U) {
        xil_printf("ALL CHECKS PASSED\r\n");
    } else {
        xil_printf("%u CHECK(S) FAILED\r\n", failures);
    }

    /* Bare metal: nothing to return to. */
    for (;;) {
        ;
    }
}
