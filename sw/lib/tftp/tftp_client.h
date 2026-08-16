/*
 * Minimal TFTP read client (RFC 1350), public contract.
 *
 * The BSP does not give us this. lwip211_v1_6 ships src/apps/tftp/tftp_server.c
 * and nothing else — tftp_init(const struct tftp_context *) is the *server*
 * entry point, there is no client counterpart, and a grep for tftp_client /
 * tftp_init_client across the whole library returns zero hits. So the read side
 * is ours.
 *
 * Scope, deliberately: read (RRQ) only, octet mode only, 512-byte blocks only,
 * no RFC 2347 option negotiation. That is exactly the subset needed to fetch
 * update.sha and update.bin, and no more. Everything inside that subset
 * is handled properly — unknown TIDs, duplicate blocks, retransmits, the
 * zero-length final block, and the post-transfer dally.
 */

#ifndef TFTP_CLIENT_H
#define TFTP_CLIENT_H

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "xil_types.h"

/*
 * Every way a fetch can end. Distinct causes stay distinct, so that when the
 * updater aborts the console says which of these happened rather than
 * "TFTP failed".
 */
typedef enum {
	TFTP_OK = 0,        /* file transferred, *out_len is its length          */
	TFTP_ERR_ARG,       /* caller passed a null pointer or a zero-length buf */
	TFTP_ERR_PCB,       /* udp_new/udp_bind/send failed — out of PCBs or mem */
	TFTP_ERR_TIMEOUT,   /* no reply after TFTP_MAX_RETRIES retransmits       */
	TFTP_ERR_SERVER,    /* server sent an ERROR packet (opcode 5)            */
	TFTP_ERR_PROTO,     /* malformed packet, or an opcode/block we can't use */
	TFTP_ERR_TOOBIG     /* file is larger than dst_len — nothing partial     */
} tftp_result_t;

/* Wire constants, RFC 1350 §5. Here rather than in the .c so a caller sizing a
 * buffer can reason about block boundaries without opening the implementation. */
#define TFTP_SERVER_PORT   69U   /* well-known port — for the RRQ only, see .c */
#define TFTP_BLOCK_SIZE    512U  /* fixed without option negotiation           */
#define TFTP_TIMEOUT_TICKS 4U    /* platform ticks per retry; tick = 250 ms    */
#define TFTP_DALLY_TICKS   4U    /* linger after the final ACK; see the .c     */

/*
 * Two retry budgets, because the two situations are not the same one.
 *
 * Mid-transfer, a timeout means a packet was lost on a link we have already
 * proved works — the server answered, so five retries is a real protocol
 * timeout and failing fast is right.
 *
 * The RRQ is different: it is sent into a link nothing has crossed yet. This is
 * the first app in this project that transmits at boot rather than waiting to
 * be contacted (the echo server is passive, so a human typing `ping` gave the
 * PHY and the switch all the time they needed and the settling was invisible).
 * A 100BASE-TX autonegotiation plus whatever the switch does before it forwards
 * can easily outlast six seconds, and every RRQ sent into that window is lost
 * with no reply — indistinguishable, from here, from a server that is down.
 *
 * So the request phase retries for about thirty seconds. Each retry is a real
 * probe rather than a blind delay, so a link that comes up in two seconds costs
 * two seconds; only a genuinely absent server pays the full budget.
 */
#define TFTP_MAX_RETRIES     5U   /* mid-transfer: link already proven          */
#define TFTP_RRQ_MAX_RETRIES 30U  /* request phase: link not yet proven         */

/*
 * Fetch `filename` from `server` into `dst`, blocking until the transfer ends.
 *
 * Blocking here means "spins the lwIP receive path itself" — there is no OS and
 * no second thread, so this function calls xemacif_input() while it waits.
 * `netif` is the interface it pumps.
 *
 * dst_len is the capacity of dst. A file that would exceed it fails with
 * TFTP_ERR_TOOBIG rather than writing a truncated prefix and reporting success.
 * On TFTP_OK, *out_len holds the byte count actually written; on anything else
 * *out_len is 0 and the contents of dst are undefined.
 *
 * Safe to call repeatedly — all per-transfer state is reset on entry and the
 * UDP PCB is released on every exit path, success or failure.
 */
tftp_result_t tftp_get(struct netif *netif,
                       const ip_addr_t *server,
                       const char *filename,
                       u8_t *dst,
                       u32_t dst_len,
                       u32_t *out_len);

/* Human-readable form of a tftp_result_t, for the console log. Never NULL. */
const char *tftp_result_str(tftp_result_t r);

#endif /* TFTP_CLIENT_H */
