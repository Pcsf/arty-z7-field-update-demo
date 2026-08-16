/*
 * Zynq-7000 MultiBoot redirect, public contract.
 *
 * The BootROM reads devcfg.MULTIBOOT_ADDR on every non-POR reset and fetches its
 * boot image from `MULTIBOOT_ADDR * 32 KiB` instead of from offset 0. The
 * register survives a soft reset and is cleared by a power-on reset, which is
 * what makes golden the default by construction. A redirect is therefore: write
 * the slot index, soft-reset, do not come back.
 *
 * NEVER TEST THE RAW REGISTER FOR ZERO. After a normal QSPI boot from offset 0,
 * MULTIBOOT_ADDR reads 0x0000C000 on this silicon: the address field, bits
 * [12:0], is correctly 0, but bits 14 and 15 are set by the BootROM. No vendor
 * header defines a field above bit 12 and the meaning of those two bits is
 * unknown here. So `MULTIBOOT_ADDR == 0` answers NO on a golden boot. Use
 * multiboot_slot_index(), which masks; the vendor FSBL likewise masks at all
 * three of its use sites and compares nothing raw.
 *
 * The field is 13 bits — 8192 slots, 256 MiB of reach. A larger offset would
 * silently alias, so multiboot_arm() range-checks rather than truncating.
 *
 * Constant provenance, traced to the vendor code that runs on this silicon
 * rather than to UG585 prose:
 *
 *   SLCR base      0xF8000000  XPS_SYS_CTRL_BASEADDR       xparameters_ps.h:161
 *   devcfg base    0xF8007000  XPS_DEV_CFG_APB_BASEADDR    xparameters_ps.h:168
 *   MULTIBOOT off  0x2C        XDCFG_MULTIBOOT_ADDR_OFFSET xdevcfg_hw.h:62
 *   unlock         SLCR+0x08   SlcrUnlock()  0xDF0DDF0D    fsbl.h:475
 *   lock           SLCR+0x04   SlcrLock()    0x767B767B    fsbl.h:476
 *   address mask   0x1FFF      PCAP_MBOOT_REG_REBOOT_OFFSET_MASK  pcap.h:45
 *   slot unit      0x8000      GOLDEN_IMAGE_OFFSET         fsbl.h:431
 *
 * PSS_RST_CTRL at SLCR+0x200 is the exception: it is defined nowhere in the BSP
 * or FSBL. It is corroborated only structurally — the BSP defines the whole
 * reset-control block around it (DMAC 0x20C, USB 0x210, GEM 0x214, ... OCM 0x238,
 * xil_misc_psreset_api.h) — so treat it as less certain than the rest.
 */

#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "xil_types.h"

/* fsbl.h:431 — the multiplier between a slot index and a flash offset. */
#define MB_SLOT_UNIT     0x8000U

/* pcap.h:45 — MULTIBOOT_ADDR is bits [12:0]. */
#define MB_ADDR_MASK     0x1FFFU

/* The largest flash offset a 13-bit index can name. */
#define MB_MAX_OFFSET    (MB_ADDR_MASK * MB_SLOT_UNIT)

typedef enum {
	MB_OK = 0,
	MB_ERR_ALIGN,   /* offset is not a multiple of 32 KiB — cannot be named  */
	MB_ERR_RANGE    /* offset needs more than 13 bits — would silently alias */
} mb_result_t;

/*
 * Raw MULTIBOOT_ADDR, all 32 bits. FOR REPORTING ONLY — the upper bits are not
 * zero on a golden boot, so any comparison against this value is wrong.
 */
u32 multiboot_read_raw(void);

/* The slot index the BootROM used: bits [12:0], masked. Zero means golden. */
u32 multiboot_slot_index(void);

/*
 * The flash offset the BootROM used to find the running image.
 *
 * Trustworthy on a normal boot: the stock FSBL's Update_MultiBootRegister() does
 * increment this register, but its one caller is inside FsblFallback(), an error
 * path. A successful boot leaves the register as the BootROM set it.
 */
u32 multiboot_current_offset(void);

/* Non-zero when SLCR writes are being ignored. Reads SLCR.LOCKSTA (+0x0C). */
int multiboot_slcr_is_locked(void);

void multiboot_slcr_unlock(void);
void multiboot_slcr_lock(void);

/*
 * Point the next boot at `flash_offset` WITHOUT resetting, so the register write
 * can be read back and checked before anything reboots.
 *
 * Read-modify-write: bits above [12:0] are preserved rather than zeroed, since
 * they are reserved rather than known-unused.
 */
mb_result_t multiboot_arm(u32 flash_offset);

/*
 * Arm and soft-reset. DOES NOT RETURN on success.
 *
 * Returns only when the offset is rejected, before anything is written, so a
 * caller that gets a return value knows nothing happened. The success path ends
 * in an infinite loop because the reset is not instantaneous.
 */
mb_result_t multiboot_to(u32 flash_offset);

const char *mb_result_str(mb_result_t r);

#endif /* MULTIBOOT_H */
