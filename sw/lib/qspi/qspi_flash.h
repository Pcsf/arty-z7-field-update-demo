/*
 * QSPI serial-NOR flash driver, public contract.
 *
 * ON NOT ASSUMING THE PART
 *
 * Nothing here is manufacturer-specific. Every command used is one that every
 * serial NOR part in this class implements identically:
 *
 *   0x9F RDID   read JEDEC identification (3 bytes)
 *   0x03 READ   read data, 3 address bytes, no dummy cycle
 *   0x02 PP     page program, 3 address bytes, <=256 bytes of data
 *   0xD8 SE     sector erase, 3 address bytes, 64 KB
 *   0x06 WREN   write enable — required before every PP and SE
 *   0x05 RDSR   read status register — bit 0 is WIP
 *
 * Anything part-specific (fast reads, 4-byte addressing, quad program, block
 * protection) is deliberately absent: 16 MB is fully addressable in three
 * address bytes, and the update path is bounded by a 100BASE-TX TFTP fetch
 * rather than by flash throughput.
 *
 * THE WRITE GUARD — THE POINT OF THIS FILE
 *
 * The golden image must be physically unwritable by the updater. Every erase and
 * every program is range-checked against [QSPI_WRITE_FLOOR, QSPI_WRITE_CEIL)
 * before a single byte reaches the controller. There is no unchecked entry
 * point, and there is deliberately no "force" argument: a flag that can be
 * passed is a rule that can be broken by a typo.
 *
 * Reads are unrestricted. Reading is harmless, and the updater needs to read
 * both the golden header and the boot-state sector.
 */

#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include "xil_types.h"

/*
 * Distinct causes stay distinct: when the updater aborts, the console has to say
 * which of these happened, not "flash error".
 */
typedef enum {
	QSPI_OK = 0,        /* operation completed                                */
	QSPI_ERR_ARG,       /* null pointer, zero length, or a misaligned erase   */
	QSPI_ERR_INIT,      /* LookupConfig/CfgInitialize failed — no controller  */
	QSPI_ERR_RANGE,     /* THE GUARD: write outside the permitted window      */
	QSPI_ERR_XFER,      /* XQspiPs_PolledTransfer reported a failure          */
	QSPI_ERR_TIMEOUT,   /* WIP never cleared — erase or program did not finish*/
	QSPI_ERR_STATE      /* called before qspi_flash_init() succeeded          */
} qspi_result_t;

/* Part geometry. Universal for 16 MB serial NOR in this class; the capacity
 * byte read back by qspi_flash_read_id() confirms it on real silicon. */
#define QSPI_PAGE_SIZE      256U                 /* page program granularity   */
#define QSPI_SECTOR_SIZE    (64U * 1024U)        /* sector erase granularity   */
#define QSPI_FLASH_SIZE     (16U * 1024U * 1024U)

/*
 * The writable window. Flash map:
 *
 *   0x000000  GOLDEN BOOT.BIN     <- never writable by this code
 *   0x700000  UPDATE BOOT.BIN     <- the window
 *   0xFF0000  BOOT STATE sector   <- reached only through the state accessors
 *
 * Both ends matter. The floor protects the golden image, which is the whole
 * anti-brick argument. The ceiling keeps image writes off the state sector.
 */
#define QSPI_WRITE_FLOOR    0x00700000U
#define QSPI_WRITE_CEIL     0x00FF0000U

/*
 * THE SECOND WINDOW — the boot-state sector.
 *
 * The arbiter and the updater both write boot state, and the window above
 * deliberately refuses it. The obvious fix — raise QSPI_WRITE_CEIL to 0x1000000
 * and let one function serve both regions — is wrong: it would let a length
 * miscalculation in the image writer run off the end of the update slot and
 * scribble the record that decides what boots, turning a bad update into an
 * unbootable board. The two regions want different failure modes, so they get
 * different functions.
 *
 * The state accessors take no absolute address, so there is no argument a caller
 * can get wrong that aims them at an image. That separation is structural rather
 * than checked, which is stronger than the guard.
 */
#define QSPI_STATE_ADDR     0x00FF0000U   /* base of the boot-state sector */

/* Scratch sector for driver self-test: the last 64 KB below the state sector,
 * inside the update slot. Anything below QSPI_WRITE_FLOOR is golden. */
#define QSPI_SCRATCH_ADDR   0x00FE0000U

/*
 * Bring up the controller: look up XPAR_XQSPIPS_0_DEVICE_ID, initialise, select
 * manual start with a forced slave select and HOLD_B driven, set the clock
 * prescaler. Idempotent.
 *
 * Must succeed before any other call here; the others return QSPI_ERR_STATE.
 */
qspi_result_t qspi_flash_init(void);

/*
 * Read the JEDEC ID (0x9F). Any of the three out-pointers may be NULL.
 *
 * On a bus with nothing on it — wrong MIO mapping, unpowered part, controller
 * not enabled — this reads 0x00 or 0xFF in all three bytes. Neither is a real
 * manufacturer code, which is what makes this worth doing first: it separates
 * "the flash said something" from "the pins are dead" before any erase.
 */
qspi_result_t qspi_flash_read_id(u8 *mfr, u8 *type, u8 *capacity);

/*
 * Decode a JEDEC capacity byte to bytes. The convention is 2^n, so 0x18 is
 * 16 MiB. Returns 0 for a byte outside the plausible range rather than
 * computing a nonsense shift.
 */
u32 qspi_flash_capacity_bytes(u8 capacity_byte);

/*
 * Read `len` bytes from `addr` into `dst`. Unrestricted by the guard. Crosses
 * page and sector boundaries freely; READ streams, and only *programs* wrap.
 */
qspi_result_t qspi_flash_read(u32 addr, u8 *dst, u32 len);

/*
 * Erase the 64 KB sector containing `addr`, which must be sector-aligned —
 * unaligned is QSPI_ERR_ARG rather than a silent round-down, because a caller
 * confused about which sector it is erasing should find out here.
 *
 * Guarded: the whole sector must lie inside the write window.
 *
 * Blocks until WIP clears. A 64 KB sector erase runs to hundreds of
 * milliseconds and can reach seconds on a tired part, so the timeout is
 * generous.
 */
qspi_result_t qspi_flash_erase_sector(u32 addr);

/*
 * Program `len` bytes from `src` to `addr`.
 *
 * Splits at page boundaries as a CORRECTNESS requirement, not an optimisation:
 * page program wraps within its 256-byte page instead of continuing into the
 * next, so a single PP of 512 bytes silently overwrites the first half with the
 * second and leaves the following page erased — a successful command and
 * corrupt flash.
 *
 * Guarded: [addr, addr+len) must lie inside the write window.
 *
 * Programming only clears bits; erase first. This function does not erase,
 * because a program that silently erased would make the updater's step ordering
 * unimplementable.
 */
qspi_result_t qspi_flash_program(u32 addr, const u8 *src, u32 len);

/*
 * Reconfigure the controller unconditionally, discarding the cached "already
 * initialised" state. For callers that do not own the controller for their whole
 * life — specifically the FSBL arbiter, which borrows one the FSBL has put in
 * LQSPI linear mode. Ordinary applications want qspi_flash_init().
 */
qspi_result_t qspi_flash_force_reinit(void);

/*
 * The boot-state sector — see THE SECOND WINDOW above.
 *
 * Erase takes no argument: there is exactly one state sector. Program and read
 * take an offset *within* that sector, bounded to QSPI_SECTOR_SIZE, so no caller
 * can express an address in an image region. These do NOT pass through the
 * update-slot guard, which would reject them. Program only clears bits, so the
 * record is rewritten as erase-then-program rather than in place.
 */
qspi_result_t qspi_flash_state_erase(void);
qspi_result_t qspi_flash_state_program(u32 sector_off, const u8 *src, u32 len);
qspi_result_t qspi_flash_state_read(u32 sector_off, u8 *dst, u32 len);

/* Human-readable form of a qspi_result_t, for the console log. Never NULL. */
const char *qspi_result_str(qspi_result_t r);

#endif /* QSPI_FLASH_H */
