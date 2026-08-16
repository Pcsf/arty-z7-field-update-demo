/*
 * boot_state.c — the record that decides what boots.
 *
 * Contract and rationale: boot_state.h and doc/icd_boot_arbiter.md.
 *
 * The CRC here is CRC-32/ISO-HDLC (reflected, polynomial 0xEDB88320, init and
 * final xor 0xFFFFFFFF) — the same one `cksum -a crc32` and zlib produce, so a
 * record can be checked from the host against a hex dump of the sector rather
 * than only by the code that wrote it.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "boot_state.h"
#include "qspi_flash.h"

/* CRC32 lookup table, built on first use */
static uint32_t crc32_table[256];
static int crc32_table_generated = 0;

static void generate_crc32_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_table_generated = 1;
}

uint32_t compute_crc32(const uint8_t *data, uint32_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    if (!crc32_table_generated) generate_crc32_table();

    for (uint32_t i = 0; i < length; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

int validate_boot_state(const void *state) {
    const boot_state_t *st = (const boot_state_t *)state;
    uint32_t stored_crc;

    if (st == NULL || st->magic != BOOT_STATE_MAGIC) {
        return 0;
    }

    stored_crc = st->crc32;
    return stored_crc == compute_crc32((const uint8_t *)st, offsetof(boot_state_t, crc32));
}

/*
 * The safe default, in one place so both the corrupt path and the flash-failure
 * path produce the identical record. "No update installed, no attempts made" is
 * the state of a board that has only ever been factory-programmed, and it is
 * the state every failure here is required to look like.
 */
static void fill_default(boot_state_t *out) {
    out->magic             = BOOT_STATE_MAGIC;
    out->update_present    = 0U;
    out->boot_attempts     = 0U;
    out->installed_version = 0U;
    out->crc32             = compute_crc32((const uint8_t *)out,
                                           offsetof(boot_state_t, crc32));
}

bs_result_t boot_state_read(boot_state_t *out) {
    boot_state_t  raw;
    qspi_result_t qr;

    if (out == NULL) {
        return BS_ERR_ARG;
    }

    /* Offset 0 of the state sector. The accessor takes a sector offset, not an
     * address, so there is no way to spell "somewhere in the golden image"
     * here even by accident. */
    qr = qspi_flash_state_read(0U, (uint8_t *)&raw, (uint32_t)sizeof(raw));
    if (qr != QSPI_OK) {
        /* Default even on failure: a caller that ignores this return value must
         * still end up booting golden rather than following a garbage record. */
        fill_default(out);
        return BS_ERR_FLASH;
    }

    if (!validate_boot_state(&raw)) {
        /*
         * Name the previous format rather than calling it corruption. The record
         * still defaults -- an older record has no installed_version and its CRC
         * covers a shorter struct, so there is nothing here worth salvaging --
         * but "the firmware changed" and "the flash rotted" are different
         * stories and an operator should not have to guess which one they are
         * reading. Re-running UPDATE restores the record in the current format.
         */
        int legacy = (raw.magic == BOOT_STATE_MAGIC_V1);
        fill_default(out);
        return legacy ? BS_LEGACY : BS_DEFAULTED;
    }

    *out = raw;
    return BS_OK;
}

bs_result_t boot_state_write(const boot_state_t *in) {
    boot_state_t  rec;
    qspi_result_t qr;

    if (in == NULL) {
        return BS_ERR_ARG;
    }

    /* Copy before stamping the CRC: the caller's record is not modified, and the
     * CRC always covers exactly the three fields that were about to be written
     * rather than whatever the caller happened to leave in crc32. */
    rec        = *in;
    rec.magic  = BOOT_STATE_MAGIC;
    rec.crc32  = compute_crc32((const uint8_t *)&rec,
                               offsetof(boot_state_t, crc32));

    qr = qspi_flash_state_erase();
    if (qr != QSPI_OK) {
        return BS_ERR_FLASH;
    }

    qr = qspi_flash_state_program(0U, (const uint8_t *)&rec,
                                  (uint32_t)sizeof(rec));
    if (qr != QSPI_OK) {
        return BS_ERR_FLASH;
    }

    return BS_OK;
}

const char *bs_result_str(bs_result_t r) {
    switch (r) {
    case BS_OK:        return "ok";
    case BS_DEFAULTED: return "absent or corrupt - defaulted to golden";
    /* Terse on purpose: this string is carried inside the STATUS reply, which is
     * bounded at CTRL_REPLY_MAX, and a longer form overflows it and truncates the
     * reply mid-word. The advice is printed separately by the caller. */
    case BS_LEGACY:    return "previous record format";
    case BS_ERR_FLASH: return "flash access failed";
    case BS_ERR_ARG:   return "bad argument";
    default:           return "unknown";
    }
}
