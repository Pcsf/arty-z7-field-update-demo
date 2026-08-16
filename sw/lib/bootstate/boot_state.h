/*
 * boot_state.h — the record that decides what boots, and its flash layer.
 *
 * Contract: doc/icd_boot_arbiter.md § Boot State Record.
 *
 * One record lives in the dedicated 64 KB QSPI sector at QSPI_STATE_ADDR
 * (0xFF0000). Two writers touch it, at different times: the FSBL arbiter
 * increments boot_attempts on every update boot, and the updater application
 * clears update_present before flashing and commits afterwards.
 *
 * A record with bad magic or bad CRC is treated as {update_present=0,
 * boot_attempts=0}, so factory-fresh flash -- erased, all 0xFF -- boots golden
 * by construction rather than by luck. That is the most important property in
 * this file: the failure mode of an unreadable record is "boot the image that
 * was never written in the field", not "guess".
 *
 * ON WHY THIS DOES NOT USE qspi_flash_program()
 *
 * It cannot. That function guards writes to [0x700000, 0xFF0000) and this sector
 * sits at the ceiling, deliberately outside it -- see the second-window comment
 * in qspi_flash.h. The state sector has its own accessors, which take no
 * absolute address at all, so no bug here can reach an image and no bug in the
 * image writer can reach this record.
 */

#ifndef BOOT_STATE_H
#define BOOT_STATE_H

#include <stdint.h>

/*
 * The magic doubles as a format version. A record in the older shape would fail
 * the CRC, which is correct but indistinguishable from corruption; a distinct
 * magic lets the reader say which of the two it found. Both still default to
 * booting golden.
 */
#define BOOT_STATE_MAGIC      0xB007CAF2U   /* current: carries installed_version */
#define BOOT_STATE_MAGIC_V1   0xB007CAFEU   /* previous: three fields, no version */
#define BOOT_ATTEMPTS_MAX     3

typedef struct {
    uint32_t magic;             /* BOOT_STATE_MAGIC */
    uint32_t update_present;    /* 1 = update slot verified good */
    uint32_t boot_attempts;     /* cleared by healthy update app */
    /*
     * The version the manifest DECLARED for the image now in the update slot,
     * stamped at commit. Meaningless when update_present is 0.
     *
     * A DECLARATION, NOT A MEASUREMENT, and every user of it must know that.
     * The payload's real VERSION lives in an AXI register that cannot be read
     * until the image has been flashed and booted, so nothing can check the
     * manifest's claim at install time. It is good enough for "should I bother
     * installing this" and must never be load-bearing for anything else.
     */
    uint32_t installed_version;
    uint32_t crc32;             /* over every field above */
} boot_state_t;

/*
 * BS_DEFAULTED is NOT an error: it is the factory-fresh path and the
 * corrupt-record path, and both are supposed to end up booting golden.
 */
typedef enum {
    BS_OK = 0,        /* record read and valid                               */
    BS_DEFAULTED,     /* record absent or corrupt; *out is the safe default  */
    BS_LEGACY,        /* previous-format record; ALSO defaulted, but knowably */
    BS_ERR_FLASH,     /* the flash layer failed                              */
    BS_ERR_ARG        /* null pointer                                        */
} bs_result_t;

/* CRC-32 (IEEE, reflected polynomial 0xEDB88320) over a byte buffer */
uint32_t compute_crc32(const uint8_t *data, uint32_t length);

/* 1 if magic and CRC are valid, 0 otherwise */
int validate_boot_state(const void *state);

/*
 * Read the record. On BS_OK *out is the stored record. On BS_DEFAULTED *out is
 * {magic, 0, 0, valid-crc} and the caller should behave exactly as it would for
 * a board with no update installed. On BS_ERR_FLASH *out is ALSO the safe
 * default -- a caller that ignores the return value still cannot be steered
 * into the update slot by a flash failure.
 *
 * qspi_flash_init() must have succeeded first.
 */
bs_result_t boot_state_read(boot_state_t *out);

/*
 * Recompute the CRC over *in, erase the state sector, and program the record.
 *
 * Erase-then-program because flash programming only clears bits; there is no
 * in-place update of a record whose fields move in both directions. One 64 KB
 * erase per call, which is what the ICD's endurance note counts.
 *
 * Not atomic, and nothing here pretends otherwise: power lost between the erase
 * and the program leaves an erased sector, which reads back as BS_DEFAULTED and
 * therefore boots golden. That is the right outcome for both callers -- the
 * updater has already invalidated the slot by the time it writes, and the
 * arbiter losing an increment costs one extra attempt, not a wrong decision.
 */
bs_result_t boot_state_write(const boot_state_t *in);

/* Human-readable form, for the console log. Never NULL. */
const char *bs_result_str(bs_result_t r);

#endif /* BOOT_STATE_H */
