/*
 * sha256.c — FIPS 180-4 SHA-256.
 *
 * Written from the standard rather than vendored: either way the sixty-eight
 * constants below have to be trusted, and the only honest way to trust them is a
 * known-answer test running on the target. Given that test exists, writing it out
 * means no third-party provenance to track and an implementation that can be read
 * line-by-line against the spec.
 *
 * See sha256.h for the contract and why the primitive is streaming.
 *
 * SECTION REFERENCES ARE TO FIPS 180-4:
 *   §4.1.2  Ch, Maj, the four sigma functions
 *   §4.2.2  the sixty-four round constants K
 *   §5.1.1  padding: 0x80, zeros to 56 mod 64, 64-bit big-endian bit length
 *   §5.3.3  the eight initial hash values
 *   §6.2.2  the message schedule and the compression function
 */

#include <string.h>

#include "sha256.h"

/*
 * §4.2.2 — the first thirty-two bits of the fractional parts of the cube roots
 * of the first sixty-four primes.
 *
 * A single wrong digit anywhere in this table yields a digest that is wrong for
 * every input and looks perfectly random for all of them. It is caught by the
 * "abc" vector in main.c, which is the second thing the app checks.
 */
static const u32 K[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

/* §4.1.2. ROTR is a rotate, not a shift — the (32 - n) half is what a careless
 * transcription drops, and it produces a digest that is wrong everywhere. */
#define ROTR(x, n)   (((x) >> (n)) | ((x) << (32U - (n))))

#define CH(x, y, z)  (((x) & (y)) ^ ((~(x)) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define BSIG0(x)     (ROTR(x,  2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x)     (ROTR(x,  6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x)     (ROTR(x,  7) ^ ROTR(x, 18) ^ ((x) >>  3))
#define SSIG1(x)     (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/*
 * §6.2.2 — compress one 64-byte block into the state.
 *
 * The message schedule is built byte-wise from `block` rather than by casting
 * it to a u32 pointer. Two reasons, both real on this target: the standard
 * defines the words as big-endian and the A9 is little-endian, so a cast would
 * need a byte swap anyway; and `block` can point straight into a caller's
 * unaligned buffer, where a u32 load can fault outright.
 */
static void
sha256_compress(u32 state[8], const u8 block[SHA256_BLOCK_LEN])
{
	u32 w[64];
	u32 a, b, c, d, e, f, g, h;
	u32 t1, t2;
	u32 i;

	for (i = 0U; i < 16U; i++) {
		w[i] = ((u32)block[i * 4U + 0U] << 24) |
		       ((u32)block[i * 4U + 1U] << 16) |
		       ((u32)block[i * 4U + 2U] <<  8) |
		       ((u32)block[i * 4U + 3U]);
	}
	for (i = 16U; i < 64U; i++) {
		w[i] = SSIG1(w[i - 2U]) + w[i - 7U] + SSIG0(w[i - 15U]) + w[i - 16U];
	}

	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];

	for (i = 0U; i < 64U; i++) {
		t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
		t2 = BSIG0(a) + MAJ(a, b, c);

		h = g; g = f; f = e;
		e = d + t1;
		d = c; c = b; b = a;
		a = t1 + t2;
	}

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void
sha256_init(sha256_ctx_t *ctx)
{
	if (ctx == NULL) {
		return;
	}

	/* §5.3.3 — the fractional parts of the square roots of the first eight
	 * primes. Wrong here and every digest is wrong; caught by the same KAT. */
	ctx->state[0] = 0x6a09e667U;
	ctx->state[1] = 0xbb67ae85U;
	ctx->state[2] = 0x3c6ef372U;
	ctx->state[3] = 0xa54ff53aU;
	ctx->state[4] = 0x510e527fU;
	ctx->state[5] = 0x9b05688cU;
	ctx->state[6] = 0x1f83d9abU;
	ctx->state[7] = 0x5be0cd19U;

	ctx->bitlen = 0U;
	ctx->buflen = 0U;
}

void
sha256_update(sha256_ctx_t *ctx, const void *data, u32 len)
{
	const u8 *p = (const u8 *)data;
	u32       n;

	if (ctx == NULL || (p == NULL && len > 0U)) {
		return;
	}

	/*
	 * Three phases, and the reason the digest cannot depend on chunking:
	 * whatever the caller hands over is first used to top up the carried
	 * partial block, then consumed 64 bytes at a time, then whatever is left
	 * becomes the new partial block. The compression function therefore sees
	 * exactly the same sequence of 64-byte blocks no matter where the caller's
	 * boundaries fell.
	 */

	/* 1. Top up the partial block, and compress it if it fills. */
	if (ctx->buflen > 0U) {
		n = SHA256_BLOCK_LEN - ctx->buflen;
		if (n > len) {
			n = len;
		}
		memcpy(&ctx->buf[ctx->buflen], p, n);
		ctx->buflen += n;
		p           += n;
		len         -= n;
		ctx->bitlen += (u64)n * 8U;

		if (ctx->buflen == SHA256_BLOCK_LEN) {
			sha256_compress(ctx->state, ctx->buf);
			ctx->buflen = 0U;
		}
	}

	/* 2. Whole blocks straight from the caller's buffer — no copy, and no
	 *    alignment requirement, since compress reads it byte-wise. */
	while (len >= SHA256_BLOCK_LEN) {
		sha256_compress(ctx->state, p);
		p           += SHA256_BLOCK_LEN;
		len         -= SHA256_BLOCK_LEN;
		ctx->bitlen += (u64)SHA256_BLOCK_LEN * 8U;
	}

	/* 3. Carry the remainder. */
	if (len > 0U) {
		memcpy(&ctx->buf[ctx->buflen], p, len);
		ctx->buflen += len;
		ctx->bitlen += (u64)len * 8U;
	}
}

void
sha256_final(sha256_ctx_t *ctx, u8 digest[SHA256_DIGEST_LEN])
{
	u64 bitlen;
	u32 i;

	if (ctx == NULL || digest == NULL) {
		return;
	}

	bitlen = ctx->bitlen;

	/*
	 * §5.1.1 — append 0x80, then zeros until the block is 56 bytes, then the
	 * 64-bit big-endian message length in bits.
	 *
	 * If the 0x80 lands at offset 56 or later there is no room for the length,
	 * so this block is zero-filled and compressed, and the length goes in a
	 * second one. That is the case the 56-byte FIPS vector exists to catch, and
	 * it is the single most commonly wrong branch in a hand-written SHA-256 —
	 * every message short enough to leave room passes without it.
	 */
	ctx->buf[ctx->buflen] = 0x80U;
	ctx->buflen++;

	if (ctx->buflen > SHA256_BLOCK_LEN - 8U) {
		memset(&ctx->buf[ctx->buflen], 0, SHA256_BLOCK_LEN - ctx->buflen);
		sha256_compress(ctx->state, ctx->buf);
		ctx->buflen = 0U;
	}

	memset(&ctx->buf[ctx->buflen], 0, (SHA256_BLOCK_LEN - 8U) - ctx->buflen);

	for (i = 0U; i < 8U; i++) {
		ctx->buf[SHA256_BLOCK_LEN - 1U - i] = (u8)(bitlen >> (8U * i));
	}
	sha256_compress(ctx->state, ctx->buf);

	/* Big-endian, so the bytes come out in the order sha256sum prints them. */
	for (i = 0U; i < 8U; i++) {
		digest[i * 4U + 0U] = (u8)(ctx->state[i] >> 24);
		digest[i * 4U + 1U] = (u8)(ctx->state[i] >> 16);
		digest[i * 4U + 2U] = (u8)(ctx->state[i] >>  8);
		digest[i * 4U + 3U] = (u8)(ctx->state[i]);
	}
}

void
sha256(const void *data, u32 len, u8 digest[SHA256_DIGEST_LEN])
{
	sha256_ctx_t ctx;

	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, digest);
}

void
sha256_hex(const u8 digest[SHA256_DIGEST_LEN], char out[SHA256_HEX_LEN])
{
	static const char hex[] = "0123456789abcdef";
	u32               i;

	if (digest == NULL || out == NULL) {
		return;
	}

	for (i = 0U; i < SHA256_DIGEST_LEN; i++) {
		out[i * 2U + 0U] = hex[(digest[i] >> 4) & 0x0FU];
		out[i * 2U + 1U] = hex[digest[i] & 0x0FU];
	}
	out[SHA256_DIGEST_LEN * 2U] = '\0';
}
