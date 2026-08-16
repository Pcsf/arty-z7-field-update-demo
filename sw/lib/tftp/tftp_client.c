/*
 * Minimal TFTP read client (RFC 1350).
 *
 *
 * THE WIRE FORMAT — RFC 1350 §5, every field this file builds or parses
 * ════════════════════════════════════════════════════════════════════════════
 *
 * All 16-bit fields are big-endian. Strings are NUL-terminated ASCII. There is
 * no length field anywhere: a packet's size comes from UDP.
 *
 *   RRQ  (opcode 1)   2 bytes | string    | 1 byte | string  | 1 byte
 *                     0x0001  | filename  | 0x00   | "octet" | 0x00
 *
 *   DATA (opcode 3)   2 bytes | 2 bytes   | 0..512 bytes
 *                     0x0003  | block #   | payload
 *
 *   ACK  (opcode 4)   2 bytes | 2 bytes
 *                     0x0004  | block #
 *
 *   ERR  (opcode 5)   2 bytes | 2 bytes   | string  | 1 byte
 *                     0x0005  | err code  | message | 0x00
 *
 * Headers are assembled and parsed a byte at a time rather than by casting a
 * packed struct and calling htons(). It is a few more lines and it cannot be
 * wrong about endianness or struct padding — worth it in a file that will be
 * read more often than it is written.
 *
 * Block numbers start at 1 and count DATA packets, not bytes. A DATA packet
 * whose payload is SHORTER than 512 bytes is the last one — that is the only
 * end-of-transfer signal the protocol has. A file whose length is an exact
 * multiple of 512 therefore ends with a DATA packet carrying ZERO bytes;
 * probe_1024.txt on the host exists to exercise exactly that.
 *
 * The exchange, end to end:
 *
 *     client :ephemeral  ──── RRQ "probe.txt" octet ────>  server :69
 *     client :ephemeral  <─── DATA block 1, 512 bytes ───  server :TID
 *     client :ephemeral  ──── ACK  block 1 ─────────────>  server :TID
 *     client :ephemeral  <─── DATA block 2, 256 bytes ──   server :TID
 *     client :ephemeral  ──── ACK  block 2 ─────────────>  server :TID   done
 *
 *
 * THE FIVE THINGS THAT COST PEOPLE AN EVENING, AND WHERE THEY ARE HANDLED
 * ════════════════════════════════════════════════════════════════════════════
 *
 * 1. THE SERVER ANSWERS FROM A DIFFERENT PORT. The RRQ goes to port 69. The
 *    server's DATA comes back from a freshly allocated ephemeral port — its
 *    TID — and every ACK must go THERE, not to 69. Send ACKs to 69 and the
 *    transfer stalls at block 1 forever while tcpdump shows the server happily
 *    retransmitting. Handled in tftp_recv_cb(): the TID is latched from the
 *    first DATA packet, and tftp_send_ack() only ever sends to it.
 *
 *    RFC 1350 §4 also says a packet from any *other* port gets ERROR code 5
 *    ("Unknown transfer ID") and is otherwise ignored, leaving the transfer
 *    undisturbed — tftp_send_error_tid() does that.
 *
 * 2. NOTHING ARRIVES UNLESS YOU PUMP IT. There is no OS and no RX thread. A
 *    packet reaches the udp_recv callback only while xemacif_input() is being
 *    called, so every wait loop here goes through net_pump() (sw/lib/netpump —
 *    it lived here until the control channel became a second caller). This is
 *    the structural difference between this client and the same logic written
 *    against BSD sockets, where recvfrom() blocks and the kernel pumps.
 *
 *    A useful consequence: because xemacif_input() runs from the main loop, so
 *    does the callback. There is no interrupt-context race and no locking is
 *    needed. The volatile flags in tftp_xfer_t document the handoff; they are
 *    not synchronisation.
 *
 * 3. A pbuf MAY BE CHAINED. p->payload is not the whole packet. Every read here
 *    goes through pbuf_copy_partial(), which walks the chain. A 512-byte DATA
 *    usually arrives in one pbuf on this MAC, which is exactly what would let
 *    the bug survive testing and surface on a bigger transfer later.
 *
 * 4. THE CALLBACK OWNS THE pbuf. udp_recv hands over a reference and every path
 *    out of tftp_recv_cb() frees it, rejections included. Leak it and a long
 *    transfer runs the pool dry and the stack goes quiet with no error.
 *
 * 5. A DUPLICATE DATA MEANS OUR ACK WAS LOST. Re-send the ACK for that block and
 *    do not advance — and in particular do not store the payload twice.
 *    Acknowledging a duplicate with a *new* ACK is the Sorcerer's Apprentice
 *    bug, where both sides retransmit forever and the file arrives doubled.
 *
 *
 * TIMING
 * ════════════════════════════════════════════════════════════════════════════
 * The platform's SCU-timer ISR sets TcpFastTmrFlag every tick, and the tick is
 * 250 ms (platform_zynq.c loads XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ / 8 into a
 * timer clocked at CPU/2). That flag is the timeout unit here: 4 ticks is one
 * second per retry. net_pump() owns the flag and hands back the tick count.
 *
 * The retry budget is split in two — TFTP_RRQ_MAX_RETRIES while waiting for the
 * server's first DATA, TFTP_MAX_RETRIES once it has answered. The header
 * explains why; the short version is that this is the first app in the project
 * that transmits at boot, so it is the first one that can lose its opening
 * packet to a link that has not finished coming up.
 *
 * Careful reading the vendor's comment above RESET_RX_CNTR_LIMIT in
 * platform_zynq.c — it claims the Rx-path workaround fires "every 100
 * milliseconds", but 400 ticks at 250 ms is 100 seconds. The comment is stale;
 * the tick is what the timer load value says it is.
 */

#include <string.h>

#include "tftp_client.h"
#include "net_pump.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/inet.h"
#include "lwip/etharp.h"
#include "netif/xadapter.h"
#include "xil_printf.h"

/* TFTP opcodes, host order. */
#define TFTP_OP_RRQ    1U
#define TFTP_OP_DATA   3U
#define TFTP_OP_ACK    4U
#define TFTP_OP_ERROR  5U

/* RFC 1350 §5, error codes. Only the one we generate is named. */
#define TFTP_ERRCODE_UNKNOWN_TID  5U

#define TFTP_HDR_LEN   4U    /* opcode + block/errcode                         */
#define TFTP_MAX_RRQ   128U  /* filename + "octet" + framing must fit          */
#define TFTP_MAX_ERRMSG 80U  /* server error text we bother to print           */

/*
 * State for one transfer, shared between tftp_get() and its receive callback.
 * A single instance: one transfer at a time by construction, and giving lwIP a
 * pointer to a stack frame would be a trap for the next person who adds an
 * early return. Reset wholesale at the top of every tftp_get().
 */
typedef struct {
	u8_t      *dst;           /* caller's buffer                              */
	u32_t      dst_len;       /* its capacity                                 */
	u32_t      written;       /* bytes stored so far                          */
	u16_t      block;         /* last block successfully stored               */
	ip_addr_t  peer_ip;       /* server address                               */
	u16_t      peer_tid;      /* server's ephemeral port — 0 until first DATA */
	volatile u8_t got_data;   /* a new in-order block was stored; needs an ACK */
	volatile u8_t last_block; /* that block was short — transfer ends after ACK */
	volatile u8_t resend_ack; /* duplicate seen — re-ACK, store nothing        */
	volatile tftp_result_t failed;  /* TFTP_OK while healthy                   */
} tftp_xfer_t;

static tftp_xfer_t xfer;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

const char *
tftp_result_str(tftp_result_t r)
{
	switch (r) {
	case TFTP_OK:          return "OK";
	case TFTP_ERR_ARG:     return "bad argument";
	case TFTP_ERR_PCB:     return "could not allocate/bind a UDP PCB";
	case TFTP_ERR_TIMEOUT: return "timed out (server down, wrong IP, or firewall?)";
	case TFTP_ERR_SERVER:  return "server sent ERROR (file missing? permissions?)";
	case TFTP_ERR_PROTO:   return "protocol violation from server";
	case TFTP_ERR_TOOBIG:  return "file larger than the destination buffer";
	default:               return "unknown";
	}
}

/* Ship `len` bytes of `buf` to ip:port. Frees its own pbuf on every path —
 * udp_sendto copies what it needs and does not take ownership. */
static err_t
tftp_send_raw(struct udp_pcb *pcb, const ip_addr_t *ip, u16_t port,
              const u8_t *buf, u16_t len)
{
	struct pbuf *p;
	err_t err;

	p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
	if (p == NULL) {
		return ERR_MEM;
	}
	if (pbuf_take(p, buf, len) != ERR_OK) {
		pbuf_free(p);
		return ERR_MEM;
	}
	err = udp_sendto(pcb, p, ip, port);
	pbuf_free(p);
	return err;
}

/* RRQ goes to the well-known port. This is the ONLY packet that ever does. */
static err_t
tftp_send_rrq(struct udp_pcb *pcb, const ip_addr_t *server, const char *filename)
{
	static const char mode[] = "octet";
	u8_t  buf[TFTP_MAX_RRQ];
	u16_t len = 0U;
	size_t fn_len = strlen(filename);

	/* 2 opcode + name + NUL + mode + NUL */
	if ((2U + fn_len + 1U + (sizeof(mode) - 1U) + 1U) > sizeof(buf)) {
		return ERR_ARG;
	}

	buf[len++] = 0U;
	buf[len++] = (u8_t)TFTP_OP_RRQ;
	memcpy(&buf[len], filename, fn_len);
	len = (u16_t)(len + fn_len);
	buf[len++] = 0U;
	memcpy(&buf[len], mode, sizeof(mode) - 1U);
	len = (u16_t)(len + (sizeof(mode) - 1U));
	buf[len++] = 0U;

	return tftp_send_raw(pcb, server, TFTP_SERVER_PORT, buf, len);
}

/* ACKs go to the server's TID. Never to TFTP_SERVER_PORT — see note 1. */
static err_t
tftp_send_ack(struct udp_pcb *pcb, const tftp_xfer_t *x, u16_t block)
{
	u8_t buf[TFTP_HDR_LEN];

	buf[0] = 0U;
	buf[1] = (u8_t)TFTP_OP_ACK;
	buf[2] = (u8_t)(block >> 8);
	buf[3] = (u8_t)(block & 0xFFU);

	return tftp_send_raw(pcb, &x->peer_ip, x->peer_tid, buf, sizeof(buf));
}

/*
 * Answer a datagram that arrived from the wrong TID, per RFC 1350 §4. It goes
 * back to the sender, not to our peer, and deliberately does not touch the
 * transfer state — a stray packet must not be able to disturb a healthy
 * transfer, which is the whole reason the RFC specifies a reply at all.
 */
static void
tftp_send_error_tid(struct udp_pcb *pcb, const ip_addr_t *ip, u16_t port)
{
	static const char msg[] = "Unknown transfer ID";
	u8_t  buf[TFTP_HDR_LEN + sizeof(msg)];
	u16_t len = 0U;

	buf[len++] = 0U;
	buf[len++] = (u8_t)TFTP_OP_ERROR;
	buf[len++] = 0U;
	buf[len++] = (u8_t)TFTP_ERRCODE_UNKNOWN_TID;
	memcpy(&buf[len], msg, sizeof(msg));      /* includes the NUL */
	len = (u16_t)(len + sizeof(msg));

	(void)tftp_send_raw(pcb, ip, port, buf, len);
}

/* ── The receive path ─────────────────────────────────────────────────────── */

static void
tftp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
             const ip_addr_t *addr, u16_t port)
{
	tftp_xfer_t *x = (tftp_xfer_t *)arg;
	u8_t  hdr[TFTP_HDR_LEN];
	u16_t opcode, block, payload_len;

	if (p == NULL) {
		return;
	}
	if (x == NULL) {
		pbuf_free(p);
		return;
	}

	/* Anything too short to carry a header is not TFTP. */
	if (p->tot_len < TFTP_HDR_LEN ||
	    pbuf_copy_partial(p, hdr, TFTP_HDR_LEN, 0U) != TFTP_HDR_LEN) {
		x->failed = TFTP_ERR_PROTO;
		pbuf_free(p);
		return;
	}

	opcode = (u16_t)(((u16_t)hdr[0] << 8) | hdr[1]);
	block  = (u16_t)(((u16_t)hdr[2] << 8) | hdr[3]);

	/* Only the host we sent the RRQ to gets to talk to us at all. A datagram
	 * from a different ADDRESS is not a TID mismatch, it is someone else's
	 * traffic — drop it silently rather than answering and advertising that
	 * we are here. */
	if (!ip_addr_cmp(addr, &x->peer_ip)) {
		pbuf_free(p);
		return;
	}

	if (opcode == TFTP_OP_ERROR) {
		char  msg[TFTP_MAX_ERRMSG];
		u16_t mlen = (p->tot_len > TFTP_HDR_LEN)
		             ? (u16_t)(p->tot_len - TFTP_HDR_LEN) : 0U;

		if (mlen >= sizeof(msg)) {
			mlen = (u16_t)(sizeof(msg) - 1U);
		}
		if (mlen > 0U) {
			(void)pbuf_copy_partial(p, msg, mlen, TFTP_HDR_LEN);
		}
		msg[mlen] = '\0';
		/* `block` is the error code in an ERROR packet. */
		xil_printf("TFTP: server error %d: %s\r\n", (int)block, msg);
		x->failed = TFTP_ERR_SERVER;
		pbuf_free(p);
		return;
	}

	if (opcode != TFTP_OP_DATA) {
		/* A client should never see RRQ, WRQ or ACK. */
		x->failed = TFTP_ERR_PROTO;
		pbuf_free(p);
		return;
	}

	/* Note 1: latch the server's TID from the first DATA, and defend it. */
	if (x->peer_tid == 0U) {
		x->peer_tid = port;
	} else if (port != x->peer_tid) {
		tftp_send_error_tid(pcb, addr, port);
		pbuf_free(p);
		return;
	}

	payload_len = (u16_t)(p->tot_len - TFTP_HDR_LEN);

	if (block == (u16_t)(x->block + 1U)) {
		/* The next block in sequence. u16_t arithmetic wraps exactly the way
		 * the protocol does, so a transfer past 65535 blocks (32 MiB) keeps
		 * working past the 9 MiB this project needs. */
		if (payload_len > 0U) {
			/* Note the bounds check happens BEFORE the copy, and rejects the
			 * whole transfer rather than storing a truncated prefix. */
			if (payload_len > (x->dst_len - x->written)) {
				x->failed = TFTP_ERR_TOOBIG;
				pbuf_free(p);
				return;
			}
			if (pbuf_copy_partial(p, x->dst + x->written, payload_len,
			                      TFTP_HDR_LEN) != payload_len) {
				x->failed = TFTP_ERR_PROTO;
				pbuf_free(p);
				return;
			}
			x->written += payload_len;
		}

		x->block = block;
		if (payload_len < TFTP_BLOCK_SIZE) {
			x->last_block = 1U;   /* includes the zero-length final block */
		}
		x->got_data = 1U;
	} else if (block == x->block) {
		/* Note 5: our ACK was lost. Re-ACK, store nothing, advance nothing. */
		x->resend_ack = 1U;
	} else {
		x->failed = TFTP_ERR_PROTO;
	}

	pbuf_free(p);
}

/* ── The transfer ─────────────────────────────────────────────────────────── */

tftp_result_t
tftp_get(struct netif *netif, const ip_addr_t *server, const char *filename,
         u8_t *dst, u32_t dst_len, u32_t *out_len)
{
	struct udp_pcb *pcb;
	tftp_result_t   result = TFTP_ERR_TIMEOUT;   /* every path below overwrites it */
	u32_t           retries = 0U;
	u32_t           ticks;

	if (netif == NULL || server == NULL || filename == NULL ||
	    dst == NULL || dst_len == 0U || out_len == NULL) {
		return TFTP_ERR_ARG;
	}

	*out_len = 0U;

	/* Wholesale reset — this function is called more than once per boot. */
	memset(&xfer, 0, sizeof(xfer));
	xfer.dst     = dst;
	xfer.dst_len = dst_len;
	xfer.failed  = TFTP_OK;
	ip_addr_copy(xfer.peer_ip, *server);

	pcb = udp_new();
	if (pcb == NULL) {
		return TFTP_ERR_PCB;
	}
	/* Port 0 asks lwIP for an ephemeral local port — our own TID. */
	if (udp_bind(pcb, IP_ADDR_ANY, 0) != ERR_OK) {
		udp_remove(pcb);
		return TFTP_ERR_PCB;
	}
	udp_recv(pcb, tftp_recv_cb, &xfer);

	if (tftp_send_rrq(pcb, server, filename) != ERR_OK) {
		result = TFTP_ERR_PCB;
		goto out;
	}

	for (;;) {
		ticks = 0U;
		while (!xfer.got_data && !xfer.resend_ack &&
		       xfer.failed == TFTP_OK && ticks < TFTP_TIMEOUT_TICKS) {
			ticks += net_pump(netif);
		}

		if (xfer.failed != TFTP_OK) {
			result = xfer.failed;
			goto out;
		}

		if (xfer.resend_ack) {
			xfer.resend_ack = 0U;
			(void)tftp_send_ack(pcb, &xfer, xfer.block);
			continue;
		}

		if (xfer.got_data) {
			xfer.got_data = 0U;
			retries = 0U;
			if (tftp_send_ack(pcb, &xfer, xfer.block) != ERR_OK) {
				result = TFTP_ERR_PCB;
				goto out;
			}
			if (xfer.last_block) {
				result = TFTP_OK;
				break;
			}
			continue;
		}

		/* Nothing arrived in TFTP_TIMEOUT_TICKS. Retransmit whichever packet
		 * we are waiting on a reply to: the RRQ if the server has never
		 * answered, otherwise the ACK the server presumably missed.
		 *
		 * The budget depends on which of those it is — see the two constants
		 * in the header. Before the first DATA we have no evidence the link
		 * even carries traffic yet; after it, we have. */
		retries++;

		if (xfer.peer_tid == 0U) {
			if (retries > TFTP_RRQ_MAX_RETRIES) {
				result = TFTP_ERR_TIMEOUT;
				goto out;
			}
			/* Say so, rather than sitting silent for half a minute looking
			 * hung. Every fifth attempt keeps the console readable.
			 *
			 * Report the ARP state alongside it. "Timed out" covers two very
			 * different faults that are indistinguishable from the outside:
			 * we cannot resolve the server's MAC (nothing we send ever leaves
			 * the board, because lwIP is still waiting on ARP), or we resolved
			 * it fine and the datagrams are being lost downstream. Printing the
			 * resolved MAC also catches the third case — resolved, but to the
			 * wrong host, which no amount of retrying will fix. */
			if ((retries % 5U) == 0U) {
				struct eth_addr  *mac = NULL;
				const ip4_addr_t *ip  = NULL;

				xil_printf("  no reply yet, retrying RRQ (%d/%d)",
				           (int)retries, (int)TFTP_RRQ_MAX_RETRIES);

				if (etharp_find_addr(netif, ip_2_ip4(server), &mac, &ip) >= 0 &&
				    mac != NULL) {
					xil_printf("  [server MAC %02x:%02x:%02x:%02x:%02x:%02x]\r\n",
					           mac->addr[0], mac->addr[1], mac->addr[2],
					           mac->addr[3], mac->addr[4], mac->addr[5]);
				} else {
					xil_printf("  [server MAC UNRESOLVED — ARP is the problem]\r\n");
				}
			}
			(void)tftp_send_rrq(pcb, server, filename);
		} else {
			if (retries > TFTP_MAX_RETRIES) {
				result = TFTP_ERR_TIMEOUT;
				goto out;
			}
			(void)tftp_send_ack(pcb, &xfer, xfer.block);
		}
	}

	/*
	 * The dally (RFC 1350 §6). The server cannot distinguish a lost final ACK
	 * from a lost final DATA, so it retransmits the last block and waits. If we
	 * tear down immediately we never answer, the server retransmits until its
	 * own retry limit, and the next transfer starts against a server still
	 * cleaning up the previous one.
	 *
	 * Costs one second per transfer. Skipping it works fine against tftp-hpa on
	 * a quiet LAN — which is exactly why it is easy to leave out and hard to
	 * debug when the network is not quiet.
	 */
	ticks = 0U;
	while (ticks < TFTP_DALLY_TICKS) {
		ticks += net_pump(netif);
		if (xfer.resend_ack) {
			xfer.resend_ack = 0U;
			(void)tftp_send_ack(pcb, &xfer, xfer.block);
		}
	}

out:
	udp_remove(pcb);
	if (result == TFTP_OK) {
		*out_len = xfer.written;
	}
	return result;
}
