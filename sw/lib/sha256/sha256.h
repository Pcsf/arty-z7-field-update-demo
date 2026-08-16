/*
 * SHA-256 (FIPS 180-4), public contract.
 *
 * This is what makes the updater's two integrity checks mean anything:
 *
 *   4. sha256(DDR, length) == manifest?   abort if not — flash untouched
 *   7. read back from flash, hash again == manifest?   abort if not
 *
 * Those two comparisons are the whole safety argument. Step 5 clears
 * update_present before step 6 touches the slot, so the boot arbiter never
 * trusts a slot that has not passed step 7. If the digest is wrong, every one of
 * those guarantees is decorative.
 *
 * WHY STREAMING AND NOT sha256(buffer, length)
 *
 * A one-shot shape assumes the whole image is contiguous in memory before
 * hashing starts, and the updater never has that: update.bin arrives over TFTP
 * in 512-byte blocks and is re-read from flash in chunks. So the primitive is
 * init/update/final, and the one-shot is a wrapper over it.
 *
 * The property that makes streaming trustworthy is that the digest does not
 * depend on how the caller happened to divide the data — a universal claim,
 * tested as one by the chunking check in main.c, which hashes the same 4096
 * bytes eight different ways and requires one answer.
 *
 * ON THE MAGIC NUMBERS
 *
 * The implementation carries sixty-eight constants transcribed from the
 * standard — sixty-four round constants and eight initial hash values. A single
 * mistyped hex digit produces a digest that is wrong for every input and
 * plausible for all of them. The defence is not care, it is the known-answer
 * test: the "abc" vector fails immediately on any such typo, and it runs on the
 * target rather than on a host.
 *
 * No dynamic allocation anywhere. The context is the caller's to place — on the
 * stack, in .bss, wherever — because the updater hashes from a main loop with a
 * known stack ceiling and no allocator.
 */

#ifndef SHA256_H
#define SHA256_H

#include "xil_types.h"

#define SHA256_DIGEST_LEN  32U   /* bytes, big-endian, as sha256sum prints */
#define SHA256_BLOCK_LEN   64U   /* the compression function's block size  */
#define SHA256_HEX_LEN     65U   /* 64 hex digits plus the NUL             */

/*
 * Digest state. Opaque by convention — nothing outside sha256.c should read
 * these fields — but declared here so a caller can allocate one without an
 * allocator.
 *
 * `bitlen` is a 64-bit BIT count, not a byte count. Both halves matter: the
 * padding block carries the message length in bits regardless of how short the
 * message is, and a 32-bit counter would silently wrap at 512 MiB. Nothing this
 * project hashes comes close to that, which is exactly why the wrong choice
 * here would never show up in testing.
 */
typedef struct {
	u32 state[8];                 /* H0..H7                                  */
	u64 bitlen;                   /* total message length in bits            */
	u8  buf[SHA256_BLOCK_LEN];    /* partial block carried between updates   */
	u32 buflen;                   /* bytes currently in buf                  */
} sha256_ctx_t;

/* Reset the context to the FIPS 180-4 initial hash value. */
void sha256_init(sha256_ctx_t *ctx);

/*
 * Absorb `len` bytes. Callable any number of times with any sizes — one byte at
 * a time, a 512-byte TFTP block, or a whole 4 KiB flash read — and the digest is
 * identical for the same byte sequence however it was divided.
 *
 * `data` may be unaligned. Message words are assembled byte-wise rather than by
 * casting to u32*, which is both endian-independent and safe on an A9 that can
 * fault on an unaligned word load. Flash read-back buffers are not guaranteed
 * aligned, so this is a requirement rather than a nicety.
 */
void sha256_update(sha256_ctx_t *ctx, const void *data, u32 len);

/*
 * Append the padding, absorb the length, and write the 32-byte big-endian
 * digest. The context is dead afterwards — call sha256_init() to reuse it.
 */
void sha256_final(sha256_ctx_t *ctx, u8 digest[SHA256_DIGEST_LEN]);

/* One-shot convenience over the three above, for callers that do have the whole
 * message contiguous. */
void sha256(const void *data, u32 len, u8 digest[SHA256_DIGEST_LEN]);

/*
 * Format a digest as 64 lowercase hex digits plus a NUL — byte for byte what
 * `sha256sum` prints, so a console line can be compared against the host by
 * eye as well as by code.
 */
void sha256_hex(const u8 digest[SHA256_DIGEST_LEN], char out[SHA256_HEX_LEN]);

#endif /* SHA256_H */
