/*
 * updater.c — the field-update pipeline.
 *
 * Contract: doc/icd_updater.md. Read that before changing the ORDER of
 * anything here; the ordering is the safety property, not an implementation
 * detail.
 *
 * Composed from the library modules, each of which was proven standalone on this
 * board before it appeared here.
 */

#include <string.h>

#include "xil_printf.h"
#include "xil_io.h"
#include "sleep.h"

#include "updater.h"
#include "tftp_client.h"
#include "sha256.h"
#include "qspi_flash.h"
#include "boot_state.h"

/* The payload's register map — doc/icd_blinkctl.md. blinkctl is deliberately
 * absent from xparameters.h (it is not in the block design), so the address
 * comes from the ICD, exactly as every other app in this project does it. */
#define BLINKCTL_BASE     0x43C00000U
#define BLINKCTL_VERSION  (BLINKCTL_BASE + 0x00U)
#define BLINKCTL_HEARTBEAT (BLINKCTL_BASE + 0x0CU)

/* Names on the TFTP server, per the ICD's release recipe. */
#define UPD_MANIFEST_NAME "update.sha"
#define UPD_IMAGE_NAME    "update.bin"

/* The update slot: base from the flash guard's floor, end at the state sector.
 * Taken from qspi_flash.h rather than restated, so there is one definition of
 * where the slot is. */
#define SLOT_BASE   QSPI_WRITE_FLOOR              /* 0x700000 */
#define SLOT_END    QSPI_WRITE_CEIL               /* 0xFF0000 */
#define SLOT_SIZE   (SLOT_END - SLOT_BASE)        /* 0x8F0000, 8.94 MB */

/*
 * The manifest format this build speaks. Bumped whenever a key is added or its
 * meaning changes -- see manifest_parse() for why that is a bump and not a
 * silent extension.
 */
#define MANIFEST_FORMAT  1U

typedef struct {
	u8  digest[SHA256_DIGEST_LEN];
	u32 length;
	u32 version;
} manifest_t;

/*
 * Set at every abort that knows more than its upd_result_t does, so the control
 * channel can put the same sentence on the wire that the console gets.
 */
static const char *upd_detail = "";

const char *
updater_last_detail(void)
{
	return upd_detail;
}

const char *
upd_result_str(upd_result_t r)
{
	switch (r) {
	case UPD_OK:                 return "ok";
	case UPD_SKIPPED_SAME:       return "already installed - nothing to do";
	case UPD_ERR_MANIFEST_GET:   return "step 2: manifest fetch failed";
	case UPD_ERR_MANIFEST_PARSE: return "step 2: manifest malformed";
	case UPD_ERR_DOWNGRADE:      return "step 2b: older than the installed "
	                                    "version - send UPDATE FORCE to override";
	case UPD_ERR_IMAGE_GET:      return "step 3: image fetch failed";
	case UPD_ERR_SHORT:          return "step 3: image shorter than manifest";
	case UPD_ERR_TOOBIG:         return "image does not fit the update slot";
	case UPD_ERR_HASH_DOWNLOAD:  return "step 4: hash mismatch on download";
	case UPD_ERR_NOT_BOOTIMAGE:  return "not a Zynq boot image";
	case UPD_ERR_INVALIDATE:     return "step 5: could not invalidate slot";
	case UPD_ERR_ERASE:          return "step 6: erase failed";
	case UPD_ERR_PROGRAM:        return "step 6: program failed";
	case UPD_ERR_READBACK:       return "could not read the update slot back";
	case UPD_ERR_HASH_READBACK:  return "step 7: hash mismatch on read-back";
	case UPD_ERR_COMMIT:         return "step 8: could not commit";
	default:                     return "unknown";
	}
}

/*
 * Zynq-7000 boot header. UG585 Table 6-5; offsets from the start of the image.
 *
 * The width and identification words are what the BootROM will not boot
 * without, but they do NOT identify the device family: bootgen writes the same
 * two for every Xilinx family, so an image built for ZynqMP or Versal carries
 * them unchanged and passes a check that looks only at those. The vector table
 * is what separates them -- Zynq-7000 fills 0x000..0x01F with the ARM
 * self-branch `b .`, where ZynqMP writes the AArch64 encoding 0x14000000.
 *
 * Wrong-family images are producible by accident: bootgen takes `-arch` on
 * trust, does not cross-check it against the .bif, and reports "Bootimage
 * generated successfully" either way.
 */
#define BOOTHDR_VECTOR_OFF   0x000U
#define BOOTHDR_VECTOR_ARM   0xEAFFFFFEU   /* ARM `b .` — Zynq-7000 Cortex-A9 */
#define BOOTHDR_WIDTH_OFF    0x020U
#define BOOTHDR_IDENT_OFF    0x024U
#define BOOTHDR_WIDTH_MAGIC  0xAA995566U
#define BOOTHDR_IDENT_MAGIC  0x584C4E58U   /* 'XLNX' */
#define BOOTHDR_MIN_LEN      (BOOTHDR_IDENT_OFF + 4U)

/*
 * Byte-wise so alignment is never a question: `ddr` is a u8* into a DDR buffer
 * the caller chose, and a u32 load through a cast would be undefined if it were
 * ever misaligned. Four byte loads cost nothing here and cannot be wrong.
 */
static u32
rd_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int
image_is_bootable(const u8 *img, u32 len)
{
	if (len < BOOTHDR_MIN_LEN) {
		return 0;
	}
	return (rd_le32(img + BOOTHDR_VECTOR_OFF) == BOOTHDR_VECTOR_ARM) &&
	       (rd_le32(img + BOOTHDR_WIDTH_OFF)  == BOOTHDR_WIDTH_MAGIC) &&
	       (rd_le32(img + BOOTHDR_IDENT_OFF)  == BOOTHDR_IDENT_MAGIC);
}

static int
hex_nibble(char c, u8 *out)
{
	if (c >= '0' && c <= '9') { *out = (u8)(c - '0');        return 1; }
	if (c >= 'a' && c <= 'f') { *out = (u8)(c - 'a' + 10);   return 1; }
	if (c >= 'A' && c <= 'F') { *out = (u8)(c - 'A' + 10);   return 1; }
	return 0;
}

/*
 * ── Manifest ─────────────────────────────────────────────────────────────────
 *
 * Format 1, line-oriented, one `key value` pair per line, order-independent:
 *
 *     manifest 1
 *     sha256   <64 lowercase hex>
 *     length   <decimal bytes>
 *     version  0x<8 hex>
 *
 * WHY THE FORMAT IS NAMED RATHER THAN POSITIONAL. Appending a third positional
 * line would be smaller and worse: a parser that stops after the length's digits
 * IGNORES anything that follows, so a board running older firmware would read a
 * versioned manifest, skip the version, and install anyway. Applied to a
 * signature rather than a version, that is not an inconvenience but an attack.
 *
 * So the first line names the format, and a format this build does not know is
 * refused outright. Signing becomes format 2, and a format-1 board declines it
 * loudly instead of quietly verifying nothing.
 *
 * UNKNOWN KEYS ARE REFUSED, for the same reason and not out of tidiness. An
 * ignored key is an ignored guarantee. The cost is that every added field is a
 * format bump; that cost is the feature.
 *
 * Strict on the values, too, which the old comment already argued and which
 * still holds: the length is what step 4 hashes over and what step 6 rounds up,
 * so a silently truncated value would verify a prefix and flash a fraction of an
 * image that then passes its own read-back check.
 */
#define MAN_KEY_MAX  16U

/* Which required keys have been seen, so a missing one is caught at the end. */
#define MAN_SEEN_SHA      0x1U
#define MAN_SEEN_LENGTH   0x2U
#define MAN_SEEN_VERSION  0x4U
#define MAN_SEEN_ALL      (MAN_SEEN_SHA | MAN_SEEN_LENGTH | MAN_SEEN_VERSION)

typedef enum {
	MAN_OK = 0,
	MAN_ERR_SYNTAX,      /* not key/value lines at all                       */
	MAN_ERR_FORMAT,      /* first line is not `manifest <n>`                 */
	MAN_ERR_FORMAT_VER,  /* a format version this build does not speak       */
	MAN_ERR_UNKNOWN_KEY, /* a key format 1 does not define                   */
	MAN_ERR_DUPLICATE,   /* the same key twice                               */
	MAN_ERR_VALUE,       /* a key we know, with a value we cannot use        */
	MAN_ERR_MISSING      /* a required key never appeared                    */
} man_result_t;

static const char *
man_result_str(man_result_t r)
{
	switch (r) {
	case MAN_OK:                return "ok";
	case MAN_ERR_SYNTAX:        return "not key/value lines";
	case MAN_ERR_FORMAT:        return "no 'manifest <n>' on the first line "
	                                   "(pre-format-1 manifest?)";
	case MAN_ERR_FORMAT_VER:    return "manifest format not supported by this "
	                                   "firmware";
	case MAN_ERR_UNKNOWN_KEY:   return "unknown key -- newer manifest format?";
	case MAN_ERR_DUPLICATE:     return "duplicate key";
	case MAN_ERR_VALUE:         return "malformed value";
	case MAN_ERR_MISSING:       return "a required key is missing";
	default:                    return "unknown";
	}
}

/* Advance past this line's terminator. Tolerates CRLF and a missing final LF. */
static u32
man_next_line(const u8 *buf, u32 len, u32 pos)
{
	while (pos < len && buf[pos] != '\n') {
		pos++;
	}
	return (pos < len) ? (pos + 1U) : len;
}

static int
man_key_is(const u8 *buf, u32 klen, const char *lit)
{
	u32 i;

	for (i = 0U; i < klen; i++) {
		if (lit[i] == '\0' || (char)buf[i] != lit[i]) {
			return 0;
		}
	}
	return lit[klen] == '\0';
}

/* Decimal, with the overflow guard the old parser already carried. */
static int
man_parse_dec(const u8 *buf, u32 len, u32 *out)
{
	u32 v = 0U;
	u32 i;

	if (len == 0U) {
		return 0;
	}
	for (i = 0U; i < len; i++) {
		if (buf[i] < '0' || buf[i] > '9') {
			return 0;
		}
		if (v > (0xFFFFFFFFU - (u32)(buf[i] - '0')) / 10U) {
			return 0;
		}
		v = (v * 10U) + (u32)(buf[i] - '0');
	}
	*out = v;
	return 1;
}

/* `0x` followed by 1..8 hex digits. The `0x` is required: a bare number here
 * would be ambiguous with the decimal fields above at a glance, and this value
 * is compared for ordering, where a silent base mistake is a wrong decision. */
static int
man_parse_hex32(const u8 *buf, u32 len, u32 *out)
{
	u32 v = 0U;
	u32 i;

	if (len < 3U || len > 10U || buf[0] != '0' ||
	    (buf[1] != 'x' && buf[1] != 'X')) {
		return 0;
	}
	for (i = 2U; i < len; i++) {
		u8 n;
		if (!hex_nibble((char)buf[i], &n)) {
			return 0;
		}
		v = (v << 4) | n;
	}
	*out = v;
	return 1;
}

static int
man_parse_sha(const u8 *buf, u32 len, u8 *out)
{
	u32 i;

	if (len != SHA256_HEX_LEN - 1U) {
		return 0;
	}
	for (i = 0U; i < SHA256_DIGEST_LEN; i++) {
		u8 hi, lo;
		if (!hex_nibble((char)buf[i * 2U], &hi) ||
		    !hex_nibble((char)buf[(i * 2U) + 1U], &lo)) {
			return 0;
		}
		out[i] = (u8)((hi << 4) | lo);
	}
	return 1;
}

static man_result_t
manifest_parse(const u8 *buf, u32 len, manifest_t *out)
{
	u32 pos = 0U;
	u32 seen = 0U;
	u32 fmt;

	/* ---- first line: manifest <n> ---------------------------------- */
	{
		u32 eol = pos;
		u32 klen;

		while (eol < len && buf[eol] != '\n' && buf[eol] != '\r') { eol++; }
		klen = 0U;
		while (klen < (eol - pos) && buf[pos + klen] != ' ' &&
		       buf[pos + klen] != '\t') {
			klen++;
		}
		if (!man_key_is(buf + pos, klen, "manifest")) {
			return MAN_ERR_FORMAT;
		}
		{
			u32 vs = pos + klen;
			while (vs < eol && (buf[vs] == ' ' || buf[vs] == '\t')) { vs++; }
			if (!man_parse_dec(buf + vs, eol - vs, &fmt)) {
				return MAN_ERR_FORMAT;
			}
		}
		if (fmt != MANIFEST_FORMAT) {
			return MAN_ERR_FORMAT_VER;
		}
		pos = man_next_line(buf, len, pos);
	}

	/* ---- remaining lines, any order -------------------------------- */
	while (pos < len) {
		u32 eol = pos;
		u32 klen;
		u32 vs;
		u32 vlen;

		while (eol < len && buf[eol] != '\n' && buf[eol] != '\r') { eol++; }

		/* A blank line is not a syntax error; a line with no value is. */
		if (eol == pos) {
			pos = man_next_line(buf, len, pos);
			continue;
		}

		klen = 0U;
		while (klen < (eol - pos) && buf[pos + klen] != ' ' &&
		       buf[pos + klen] != '\t') {
			klen++;
		}
		if (klen == 0U || klen > MAN_KEY_MAX) {
			return MAN_ERR_SYNTAX;
		}

		vs = pos + klen;
		while (vs < eol && (buf[vs] == ' ' || buf[vs] == '\t')) { vs++; }
		vlen = eol - vs;
		if (vlen == 0U) {
			return MAN_ERR_SYNTAX;
		}

		if (man_key_is(buf + pos, klen, "sha256")) {
			if ((seen & MAN_SEEN_SHA) != 0U)         { return MAN_ERR_DUPLICATE; }
			if (!man_parse_sha(buf + vs, vlen, out->digest)) {
				return MAN_ERR_VALUE;
			}
			seen |= MAN_SEEN_SHA;
		} else if (man_key_is(buf + pos, klen, "length")) {
			if ((seen & MAN_SEEN_LENGTH) != 0U)      { return MAN_ERR_DUPLICATE; }
			if (!man_parse_dec(buf + vs, vlen, &out->length)) {
				return MAN_ERR_VALUE;
			}
			seen |= MAN_SEEN_LENGTH;
		} else if (man_key_is(buf + pos, klen, "version")) {
			if ((seen & MAN_SEEN_VERSION) != 0U)     { return MAN_ERR_DUPLICATE; }
			if (!man_parse_hex32(buf + vs, vlen, &out->version)) {
				return MAN_ERR_VALUE;
			}
			seen |= MAN_SEEN_VERSION;
		} else {
			return MAN_ERR_UNKNOWN_KEY;
		}

		pos = man_next_line(buf, len, pos);
	}

	if (seen != MAN_SEEN_ALL) {
		return MAN_ERR_MISSING;
	}
	return MAN_OK;
}

static void
print_hex32(const u8 *d)
{
	char hex[SHA256_HEX_LEN];
	sha256_hex(d, hex);
	xil_printf("%s", hex);
}

/*
 * How much work happens between two chances for the caller to service the
 * network and the watchdog. 64 KB is one erase sector and 16 pages of program,
 * so every loop below lands on a natural boundary; at QSPI speeds it is a few
 * tens of milliseconds, far inside any timeout that matters.
 */
#define UPD_POLL_CHUNK  (64U * 1024U)

static void
upd_poll(void (*poll)(void))
{
	if (poll != NULL) {
		poll();
	}
}

u32
updater_payload_version(void)
{
	return Xil_In32(BLINKCTL_VERSION);
}

int
updater_payload_healthy(u32 stable_s, void (*kick)(void))
{
	u32 prev;
	u32 s;

	xil_printf("  payload: VERSION=0x%08x\r\n",
	           (unsigned)updater_payload_version());

	prev = Xil_In32(BLINKCTL_HEARTBEAT);

	for (s = 0U; s < stable_s; s++) {
		u32 now;

		if (kick != NULL) {
			kick();
		}
		sleep(1);

		now = Xil_In32(BLINKCTL_HEARTBEAT);
		if (now == prev) {
			/* Not moving: either the PL was never configured by this image or
			 * it is not clocked. Both mean this image must not be committed. */
			xil_printf("  payload: heartbeat STALLED at 0x%08x\r\n",
			           (unsigned)now);
			return 0;
		}
		prev = now;
	}

	xil_printf("  payload: heartbeat moving, stable %u s\r\n",
	           (unsigned)stable_s);
	return 1;
}

/*
 * SHA-256 over the first `length` bytes of the update slot, streamed out of
 * flash.
 *
 * Streamed rather than read into a second buffer, and that is load-bearing at
 * step 7: reusing the download buffer would destroy the evidence of what was
 * sent, and comparing flash against itself proves nothing.
 *
 * Step 2c calls the same function for a different question -- is what is
 * already in the slot bit-identical to what the manifest describes -- and
 * hashing over the CANDIDATE's length is correct by construction there. An
 * installed image of a different size cannot match, whether the hash runs off
 * its end into erased bytes or stops short of it; either way the digests differ
 * and the pipeline proceeds, which is the safe direction.
 */
static qspi_result_t
slot_hash(u32 length, u8 *out, void (*poll)(void))
{
	static u8     chunk[QSPI_PAGE_SIZE * 16U];
	sha256_ctx_t  ctx;
	qspi_result_t qr;
	u32           done = 0U;

	sha256_init(&ctx);
	while (done < length) {
		u32 n = (u32)sizeof(chunk);
		if (n > length - done) {
			n = length - done;
		}
		qr = qspi_flash_read(SLOT_BASE + done, chunk, n);
		if (qr != QSPI_OK) {
			return qr;
		}
		sha256_update(&ctx, chunk, n);
		done += n;
		upd_poll(poll);
	}
	sha256_final(&ctx, out);
	return QSPI_OK;
}

/*
 * The boot header as it sits in flash, for the one caller that has no DDR copy
 * to look at. Step 4b answers the same question about the bytes that arrived;
 * step 2c's reconcile has nothing but the slot.
 */
static qspi_result_t
slot_is_bootable(int *out)
{
	u8            hdr[BOOTHDR_MIN_LEN];
	qspi_result_t qr;

	qr = qspi_flash_read(SLOT_BASE, hdr, (u32)sizeof(hdr));
	if (qr != QSPI_OK) {
		return qr;
	}
	*out = image_is_bootable(hdr, (u32)sizeof(hdr));
	return QSPI_OK;
}

/*
 * Step 8, factored out because two paths reach it: the full pipeline, which has
 * just written and re-hashed the slot, and step 2c's reconcile, which found the
 * bytes already there. Both arrive holding the same evidence -- a slot hash
 * equal to the manifest digest, and a valid boot header -- so both are entitled
 * to the same record.
 *
 * boot_attempts goes back to zero either way: the slot has just been proven, and
 * a count left over from an earlier image would spend attempts that this one
 * never used.
 */
static upd_result_t
commit_slot(boot_state_t *st, u32 version)
{
	bs_result_t br;

	xil_printf("[8] update_present = 1, boot_attempts = 0, version = 0x%08x\r\n",
	           (unsigned)version);
	st->update_present    = 1U;
	st->boot_attempts     = 0U;
	/* The manifest's CLAIM, recorded so the next UPDATE can compare against it
	 * without booting anything. Never treated as verified -- see boot_state.h. */
	st->installed_version = version;

	br = boot_state_write(st);
	if (br != BS_OK) {
		xil_printf("    %s\r\n", bs_result_str(br));
		return UPD_ERR_COMMIT;
	}
	return UPD_OK;
}

upd_result_t
updater_run(struct netif *netif, const ip_addr_t *server, u8 *ddr, u32 ddr_len,
            int force, void (*poll)(void))
{
	manifest_t    man;
	tftp_result_t tr;
	qspi_result_t qr;
	bs_result_t   br;
	boot_state_t  st;
	u8            digest[SHA256_DIGEST_LEN];
	u32           got = 0U;
	u32           span;
	u32           off;
	int           bootable;

	upd_detail = "";

	/* ---- 2. manifest ------------------------------------------------- */
	xil_printf("\r\n[2] GET %s\r\n", UPD_MANIFEST_NAME);
	tr = tftp_get(netif, server, UPD_MANIFEST_NAME, ddr, ddr_len, &got);
	if (tr != TFTP_OK) {
		xil_printf("    %s\r\n", tftp_result_str(tr));
		return UPD_ERR_MANIFEST_GET;
	}
	{
		man_result_t mr = manifest_parse(ddr, got, &man);
		if (mr != MAN_OK) {
			upd_detail = man_result_str(mr);
			xil_printf("    %s\r\n", upd_detail);
			return UPD_ERR_MANIFEST_PARSE;
		}
	}
	xil_printf("    sha256=");
	print_hex32(man.digest);
	xil_printf("\r\n    length=%u\r\n    version=0x%08x\r\n",
	           (unsigned)man.length, (unsigned)man.version);

	/*
	 * Size check BEFORE anything is invalidated. An image that cannot fit must
	 * not cost a working slot -- aborting here leaves a board that still has a
	 * valid update installed, which is the whole difference between a rejected
	 * update and a broken one.
	 */
	if (man.length == 0U || man.length > SLOT_SIZE || man.length > ddr_len) {
		xil_printf("    %u bytes vs slot %u / buffer %u\r\n",
		           (unsigned)man.length, (unsigned)SLOT_SIZE,
		           (unsigned)ddr_len);
		return UPD_ERR_TOOBIG;
	}

	/*
	 * ---- 2b. is this worth installing? --------------------------------
	 *
	 * Every other check in this pipeline asks whether the bytes are INTACT. None
	 * asks whether they are WANTED -- without these two gates the board erases
	 * 4.3 MB, programs it, verifies it twice, reboots and commits, to arrive at
	 * the version it was already running.
	 *
	 * Both gates here run before step 5, which is the line that matters: step 5
	 * is where the installed update is given up, and a refusal that costs a
	 * working slot is not a refusal. Both also run before step 3, so a rejected
	 * update costs neither the download nor the flash cycle.
	 *
	 * ORDER: version first, hash second. The version test is free and the hash
	 * test reads 4.3 MB out of QSPI, so the cheap refusal goes first.
	 */
	(void)boot_state_read(&st);          /* always yields a usable record */

	if (st.update_present != 0U && man.version < st.installed_version) {
		xil_printf("[2b] version 0x%08x < installed 0x%08x\r\n",
		           (unsigned)man.version, (unsigned)st.installed_version);
		if (!force) {
			xil_printf("    REFUSED - downgrade. FLASH UNTOUCHED\r\n");
			return UPD_ERR_DOWNGRADE;
		}
		xil_printf("    FORCED - installing an older version on purpose\r\n");
	}

	/*
	 * ---- 2c. is it already there? -------------------------------------
	 *
	 * Asked by hashing the slot rather than by comparing versions, because this
	 * is the question the version cannot answer honestly: the manifest's version
	 * is a CLAIM about the image, and two different images may both claim it.
	 * The digest is the image's identity, so a match here means the slot holds
	 * these exact bytes and there is provably nothing to do.
	 *
	 * Not skipped under `force`. Re-installing bytes that are already in the
	 * slot cannot be what anyone means by forcing, and it would spend a
	 * flash-wear cycle on a no-op.
	 *
	 * A DIGEST MATCH PROVES THE BYTES, NOT THE RECORD. The two are written at
	 * different moments -- step 6 puts the image down, step 8 records it -- so a
	 * reset in between leaves a complete slot that the record calls empty. That
	 * board then refuses SWITCH for having no update, and refuses UPDATE for
	 * already having it, with no operator verb able to break the tie. So the
	 * skip is gated on the record agreeing, and where it does not, the answer is
	 * to finish the job the interrupted run started rather than to report
	 * success and leave the board stuck.
	 */
	xil_printf("[2c] hash the installed slot\r\n");
	qr = slot_hash(man.length, digest, poll);
	if (qr != QSPI_OK) {
		/* Not fatal: failing to read the slot only means the shortcut is
		 * unavailable, and the full pipeline below rewrites it anyway. */
		xil_printf("    read: %s - checking skipped\r\n", qspi_result_str(qr));
	} else if (memcmp(digest, man.digest, SHA256_DIGEST_LEN) == 0) {
		xil_printf("    match - the slot already holds this image\r\n");

		if (st.update_present != 0U && st.installed_version == man.version) {
			xil_printf("    NOTHING TO DO. FLASH UNTOUCHED\r\n");
			return UPD_SKIPPED_SAME;
		}

		xil_printf("    but the record says present=%u version=0x%08x - "
		           "completing it\r\n", (unsigned)st.update_present,
		           (unsigned)st.installed_version);

		/*
		 * Step 4b's question, asked of the slot. The hash says these bytes are
		 * the manifest's image; it does not say the manifest describes something
		 * the BootROM will load. Committing without this would arm a slot on the
		 * strength of a digest alone, which is the one thing step 8 has never
		 * done.
		 */
		qr = slot_is_bootable(&bootable);
		if (qr != QSPI_OK) {
			upd_detail = "step 2c: could not read the slot boot header";
			xil_printf("    header read: %s\r\n", qspi_result_str(qr));
			return UPD_ERR_READBACK;
		}
		if (!bootable) {
			upd_detail = "step 2c: the slot is not a Zynq boot image";
			xil_printf("    slot is not a boot image - FLASH UNTOUCHED, "
			           "record unchanged\r\n");
			return UPD_ERR_NOT_BOOTIMAGE;
		}
		xil_printf("    boot header valid\r\n");

		/* Straight to step 8: nothing to download, nothing to erase, and the
		 * slot has just been hashed and header-checked. */
		return commit_slot(&st, man.version);
	} else {
		xil_printf("    differs - proceeding\r\n");
	}

	/* ---- 3. image ---------------------------------------------------- */
	xil_printf("[3] GET %s\r\n", UPD_IMAGE_NAME);
	tr = tftp_get(netif, server, UPD_IMAGE_NAME, ddr, ddr_len, &got);
	if (tr != TFTP_OK) {
		xil_printf("    %s\r\n", tftp_result_str(tr));
		return UPD_ERR_IMAGE_GET;
	}
	xil_printf("    %u bytes into DDR\r\n", (unsigned)got);
	if (got < man.length) {
		/* Truncation. Hashing man.length bytes would read past what arrived. */
		return UPD_ERR_SHORT;
	}

	/* ---- 4. verify the download -------------------------------------- */
	xil_printf("[4] sha256 over %u bytes\r\n", (unsigned)man.length);
	{
		/* Chunked rather than one sha256() call, so the caller gets a chance to
		 * pump the stack every 64 KB. Same digest, same order, same bytes. */
		sha256_ctx_t ctx;
		u32          done = 0U;

		sha256_init(&ctx);
		while (done < man.length) {
			u32 n = UPD_POLL_CHUNK;
			if (n > man.length - done) {
				n = man.length - done;
			}
			sha256_update(&ctx, ddr + done, n);
			done += n;
			upd_poll(poll);
		}
		sha256_final(&ctx, digest);
	}
	if (memcmp(digest, man.digest, SHA256_DIGEST_LEN) != 0) {
		xil_printf("    got ");
		print_hex32(digest);
		xil_printf("\r\n    FLASH UNTOUCHED\r\n");
		return UPD_ERR_HASH_DOWNLOAD;
	}
	xil_printf("    match\r\n");

	/*
	 * ---- 4b. is it a boot image at all? -------------------------------
	 *
	 * Everything above this line checks that the bytes ARRIVED correctly; nothing
	 * checks that they are an image. An update.bin with four bytes of its boot
	 * header zeroed and a manifest regenerated to match passes every other step --
	 * the board erases a working update, writes an unbootable one, declares it
	 * good, and only the BootROM's golden search saves the boot.
	 *
	 * The check has to be HERE, after the hash and before step 5. Step 5 is
	 * where the installed update is given up; once update_present is 0 the
	 * board has already paid for the mistake, and no amount of later
	 * verification buys it back. Refusing at 4b means a bad image costs a
	 * download and nothing else.
	 *
	 * Three words, from UG585 Table 6-5:
	 *
	 *   0x000  vector table      0xEAFFFFFE  (ARM `b .`)
	 *   0x020  width detection   0xAA995566
	 *   0x024  identification    0x584C4E58  ('XLNX')
	 *
	 * The last two are what the BootROM will not boot without; the first is
	 * what makes this a ZYNQ-7000 image rather than any Xilinx image. Both
	 * magics are family-independent, so without the vector word a ZynqMP or
	 * Versal build passes here, installs, and leaves the board relying on the
	 * BootROM's golden search.
	 *
	 * Still deliberately NOT a full header validation. The header checksum at
	 * 0x048 and the partition table would catch more, but every additional
	 * field is another way to reject an image the silicon would have accepted,
	 * and the BootROM remains the authority on that.
	 */
	xil_printf("[4b] boot header\r\n");
	if (!image_is_bootable(ddr, man.length)) {
		xil_printf("    vector %08x, expected %08x\r\n",
		           (unsigned)rd_le32(ddr + BOOTHDR_VECTOR_OFF),
		           (unsigned)BOOTHDR_VECTOR_ARM);
		xil_printf("    magic %08x %08x, expected %08x %08x\r\n",
		           (unsigned)rd_le32(ddr + BOOTHDR_WIDTH_OFF),
		           (unsigned)rd_le32(ddr + BOOTHDR_IDENT_OFF),
		           (unsigned)BOOTHDR_WIDTH_MAGIC,
		           (unsigned)BOOTHDR_IDENT_MAGIC);
		upd_detail = "step 4b: the downloaded image is not a Zynq boot image";
		xil_printf("    FLASH UNTOUCHED\r\n");
		return UPD_ERR_NOT_BOOTIMAGE;
	}
	xil_printf("    valid\r\n");

	/* ---- 5. invalidate BEFORE touching the slot ---------------------- */
	xil_printf("[5] update_present = 0\r\n");
	/* `st` was read at step 2b and nothing since has written the record. */
	st.update_present = 0U;
	br = boot_state_write(&st);
	if (br != BS_OK) {
		xil_printf("    %s\r\n", bs_result_str(br));
		return UPD_ERR_INVALIDATE;
	}

	/* ---- 6. erase + program ------------------------------------------ */
	span = ((man.length + QSPI_SECTOR_SIZE - 1U) / QSPI_SECTOR_SIZE)
	       * QSPI_SECTOR_SIZE;
	xil_printf("[6] erase 0x%06x..0x%06x, program %u bytes\r\n",
	           (unsigned)SLOT_BASE, (unsigned)(SLOT_BASE + span),
	           (unsigned)man.length);

	for (off = 0U; off < span; off += QSPI_SECTOR_SIZE) {
		qr = qspi_flash_erase_sector(SLOT_BASE + off);
		if (qr != QSPI_OK) {
			xil_printf("    erase at 0x%06x: %s\r\n",
			           (unsigned)(SLOT_BASE + off), qspi_result_str(qr));
			return UPD_ERR_ERASE;
		}
		upd_poll(poll);
	}

	/* Programmed in chunks for the same reason the hash is: one 4.3 MB call
	 * would be one 20-second silence. The flash layer is unchanged -- each call
	 * is an ordinary guarded program, and the guard still refuses anything
	 * outside the slot. */
	for (off = 0U; off < man.length; off += UPD_POLL_CHUNK) {
		u32 n = UPD_POLL_CHUNK;
		if (n > man.length - off) {
			n = man.length - off;
		}
		qr = qspi_flash_program(SLOT_BASE + off, ddr + off, n);
		if (qr != QSPI_OK) {
			xil_printf("    program at 0x%06x: %s\r\n",
			           (unsigned)(SLOT_BASE + off), qspi_result_str(qr));
			return UPD_ERR_PROGRAM;
		}
		upd_poll(poll);
	}

	/* ---- 7. verify what actually landed ------------------------------ */
	xil_printf("[7] read back and re-hash\r\n");
	qr = slot_hash(man.length, digest, poll);
	if (qr != QSPI_OK) {
		upd_detail = "step 7: read-back of the freshly written slot failed";
		xil_printf("    read: %s\r\n", qspi_result_str(qr));
		return UPD_ERR_READBACK;
	}

	if (memcmp(digest, man.digest, SHA256_DIGEST_LEN) != 0) {
		xil_printf("    got ");
		print_hex32(digest);
		xil_printf("\r\n    SLOT BAD - left marked invalid\r\n");
		return UPD_ERR_HASH_READBACK;
	}
	xil_printf("    match\r\n");

	/* ---- 8. commit ---------------------------------------------------- */
	return commit_slot(&st, man.version);
}
