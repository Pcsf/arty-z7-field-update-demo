/*
 * main.c — SHA-256 known-answer tests.
 *
 * The updater compares a digest twice — once over the image in DDR before
 * touching flash, once over the image read back out of flash — and those two
 * comparisons are the reason a power cut mid-write cannot brick the board. A wrong digest does not make the updater
 * fail; it makes the updater confident.
 *
 * WHAT THIS APP CHECKS, AND WHY EACH ONE EARNS ITS PLACE
 *
 *   1. Known answers      the three FIPS 180-4 vectors. Catches a mistyped
 *                         round constant or initial value, which would
 *                         otherwise produce a wrong-but-random-looking digest
 *                         for every input.
 *   2. Padding boundaries 55/56/63/64/65 bytes. The padding rule changes
 *                         behaviour either side of "56 bytes into the block",
 *                         and every message short enough to leave room passes
 *                         without the second-block branch ever running.
 *   3. Long message       1,000,000 bytes, fed 1000 at a time. Exercises the
 *                         streaming path over many blocks and the 64-bit bit
 *                         counter together.
 *   4. Chunking invariance the same 4096 bytes hashed eight different ways.
 *                         The only universal claim here, and the one the
 *                         updater rests on — update.bin arrives in 512-byte
 *                         TFTP blocks and is re-read from flash in chunks, so
 *                         "the digest does not depend on how the caller divided
 *                         the data" IS the requirement.
 *   5. Unaligned input    flash read-back buffers are not guaranteed aligned,
 *                         and an A9 can fault on an unaligned word load.
 *   6. Host cross-check   a DDR buffer whose digest was computed by sha256sum
 *                         on the host, so the whole chain is anchored to an
 *                         oracle outside this code.
 *
 * Checks 1-3 and 6 are examples. Check 4 is the property. The QSPI example taught
 * that lesson the expensive way: the flash driver's fault was never in the data
 * it wrote, it was in how the data got divided, and the only test that could
 * have caught it up front was one that varied the division.
 *
 * EVERY EXPECTED DIGEST BELOW CAME FROM sha256sum, NOT FROM MEMORY.
 * Reproduce the whole set:
 *
 *   printf ''    | sha256sum
 *   printf 'abc' | sha256sum
 *   printf 'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq' | sha256sum
 *   for n in 55 56 63 64 65; do python3 -c "import sys;sys.stdout.write('a'*$n)" | sha256sum; done
 *   python3 -c "import sys;sys.stdout.write('a'*1000000)" | sha256sum
 *   python3 -c "import sys;sys.stdout.buffer.write(bytes((i&0xFF) for i in range(4096)))" | sha256sum
 */

#include <string.h>

#include "xil_printf.h"

#include "sha256.h"

/*
 * Where the cross-check fixture lives. A fixed DDR address rather than a static
 * array, mirroring tftp_client's download buffer at the same place — the shape
 * the updater has, where the fetched image sits at a known address and is
 * hashed in place without a copy.
 */
#define FIXTURE_ADDR   0x01000000U
#define FIXTURE_LEN    4096U

/* Fed 1000 times to make the million-byte vector without a million-byte
 * buffer — which is the point of a streaming digest. */
#define ABUF_LEN       1000U
#define ABUF_REPS      1000U

static u8   abuf[ABUF_LEN];
static char hexbuf[SHA256_HEX_LEN];

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

/*
 * Compare a digest against the host's hex string and report it either way.
 *
 * The digest is printed on every run, pass or fail. A failing check that only
 * says FAIL sends you to a debugger; one that prints both digests usually tells
 * you what happened on sight — an all-zero digest is a context that was never
 * initialised, a digest that is right for a shorter message is a length bug,
 * and a completely unrelated one is a constant.
 */
static int
digest_is(const char *what, const u8 *digest, const char *expect_hex)
{
	int ok;

	sha256_hex(digest, hexbuf);
	ok = (strcmp(hexbuf, expect_hex) == 0);

	xil_printf("    got  %s\r\n", hexbuf);
	if (!ok) {
		xil_printf("    want %s\r\n", expect_hex);
	}
	check(what, ok);

	return ok;
}

/* One-shot hash of a byte string, for the known-answer vectors. */
static void
hash_str(const char *s, u8 *digest)
{
	sha256((const u8 *)s, (u32)strlen(s), digest);
}

/* n bytes of 'a', hashed one-shot. Used for the padding-boundary set. */
static void
hash_a_run(u32 n, u8 *digest)
{
	u32 i;

	for (i = 0U; i < ABUF_LEN; i++) {
		abuf[i] = (u8)'a';
	}
	sha256(abuf, n, digest);
}

int
main(void)
{
	u8   digest[SHA256_DIGEST_LEN];
	u8   reference[SHA256_DIGEST_LEN];
	u8  *fixture = (u8 *)FIXTURE_ADDR;
	u32  i;

	/* The padding-boundary set: length, then the host's digest for that many
	 * 'a' bytes. 55 is the last length that leaves room for the length field in
	 * the same block; 56 is the first that does not. */
	static const struct {
		u32         len;
		const char *hex;
	} pad_cases[] = {
		{ 55U, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
		{ 56U, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
		{ 63U, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
		{ 64U, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
		{ 65U, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
	};

	/* The chunk sizes the invariance check uses. 1 and 3 are pathological, 63/64/65
	 * straddle the block size, 512 is a TFTP block, 4096 is one shot. */
	static const u32 chunk_sizes[] = { 1U, 3U, 63U, 64U, 65U, 127U, 512U, 4096U };

	xil_printf("\r\n--- SHA-256 ---\r\n");
	xil_printf("Fixture   : 0x%08x (%d bytes)\r\n", FIXTURE_ADDR, (int)FIXTURE_LEN);

	/* ---- 1. known answers (FIPS 180-4) --------------------------------- */

	xil_printf("\r\n1. FIPS 180-4 known-answer vectors\r\n");

	hash_str("", digest);
	digest_is("empty message", digest,
	          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

	hash_str("abc", digest);
	digest_is("\"abc\"", digest,
	          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	hash_str("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", digest);
	digest_is("56-byte multi-block vector", digest,
	          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	/* ---- 2. padding boundaries ----------------------------------------- */

	xil_printf("\r\n2. Padding boundaries (bytes of 'a')\r\n");

	for (i = 0U; i < (sizeof(pad_cases) / sizeof(pad_cases[0])); i++) {
		xil_printf("  %d bytes:\r\n", (int)pad_cases[i].len);
		hash_a_run(pad_cases[i].len, digest);
		digest_is("padding boundary", digest, pad_cases[i].hex);
	}

	/* ---- 3. long message, streamed ------------------------------------- */

	xil_printf("\r\n3. Long message: 1,000,000 x 'a', streamed 1000 at a time\r\n");
	{
		sha256_ctx_t ctx;

		for (i = 0U; i < ABUF_LEN; i++) {
			abuf[i] = (u8)'a';
		}

		sha256_init(&ctx);
		for (i = 0U; i < ABUF_REPS; i++) {
			sha256_update(&ctx, abuf, ABUF_LEN);
		}
		sha256_final(&ctx, digest);

		digest_is("1,000,000 x 'a'", digest,
		          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	}

	/* ---- build the DDR fixture ----------------------------------------- */

	for (i = 0U; i < FIXTURE_LEN; i++) {
		fixture[i] = (u8)(i & 0xFFU);
	}

	/* ---- 4. chunking invariance — the property ------------------------- */

	xil_printf("\r\n4. Chunking invariance over %d bytes\r\n", (int)FIXTURE_LEN);

	sha256(fixture, FIXTURE_LEN, reference);
	sha256_hex(reference, hexbuf);
	xil_printf("    reference (one shot): %s\r\n", hexbuf);

	for (i = 0U; i < (sizeof(chunk_sizes) / sizeof(chunk_sizes[0])); i++) {
		sha256_ctx_t ctx;
		u32          step = chunk_sizes[i];
		u32          off;

		sha256_init(&ctx);
		for (off = 0U; off < FIXTURE_LEN; off += step) {
			u32 n = FIXTURE_LEN - off;

			if (n > step) {
				n = step;
			}
			sha256_update(&ctx, &fixture[off], n);
		}
		sha256_final(&ctx, digest);

		xil_printf("  chunk %d:\r\n", (int)step);
		check("matches the one-shot digest",
		      memcmp(digest, reference, SHA256_DIGEST_LEN) == 0);
	}

	/* ---- 5. unaligned input -------------------------------------------- */

	/*
	 * Hash the same 4095 bytes twice: once from an odd address, once from a
	 * copy placed at a 4-byte boundary. Self-consistency rather than a host
	 * digest — the claim is that alignment does not change the answer, and
	 * that needs no external oracle.
	 */
	xil_printf("\r\n5. Unaligned input\r\n");
	{
		u8 *unaligned = &fixture[1];
		u8 *aligned   = &fixture[FIXTURE_LEN];   /* 4096 is 4-byte aligned */

		memcpy(aligned, unaligned, FIXTURE_LEN - 1U);

		sha256(unaligned, FIXTURE_LEN - 1U, digest);
		sha256(aligned,   FIXTURE_LEN - 1U, reference);

		xil_printf("    unaligned base 0x%08x, aligned base 0x%08x\r\n",
		           (u32)(INTPTR)unaligned, (u32)(INTPTR)aligned);
		check("unaligned digest equals aligned digest",
		      memcmp(digest, reference, SHA256_DIGEST_LEN) == 0);
	}

	/* ---- 6. host cross-check ------------------------------------------- */

	/*
	 * The fixture was overwritten past its end by the unaligned test above, so
	 * rebuild it before hashing. Cheap, and it removes a dependency between two
	 * checks that should not have one.
	 */
	for (i = 0U; i < FIXTURE_LEN; i++) {
		fixture[i] = (u8)(i & 0xFFU);
	}

	xil_printf("\r\n6. Host cross-check: %d-byte ramp in DDR\r\n", (int)FIXTURE_LEN);
	sha256(fixture, FIXTURE_LEN, digest);
	digest_is("matches sha256sum on the host", digest,
	          "c8f5d0341d54d951a71b136e6e2afcb14d11ed8489a7ae126a8fee0df6ecf193");

	xil_printf("\r\n%d / %d checks passed\r\n",
	           (int)(checks_run - checks_failed), (int)checks_run);

	if (checks_failed == 0U && checks_run > 0U) {
		xil_printf("--- SHA-256: ALL CHECKS PASSED ---\r\n");
	} else {
		xil_printf("--- SHA-256: FAIL ---\r\n");
	}

	/* Idle rather than return — see the note in the qspi and tftp_client apps. */
	while (1) {
		;
	}

	return 0;
}
