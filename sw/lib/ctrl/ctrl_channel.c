/*
 * ctrl_channel.c — the UDP command channel.
 *
 * Contract: doc/icd_control_channel.md.
 *
 *
 * THE ONE RULE: THE CALLBACK RECORDS, THE MAIN LOOP ACTS
 * ════════════════════════════════════════════════════════════════════════════
 *
 * ctrl_recv_cb() may parse, may record one pending verb, and may send one
 * bounded reply. It may not touch flash, may not call the updater pipeline, may
 * not redirect, and may not wait for anything.
 *
 * This is not style. UPDATE takes about a minute. Run it inside the callback and
 * the stack stops draining for that minute -- the EMAC ISR keeps allocating
 * pbufs for whatever the LAN is broadcasting, nothing frees them, and the pool
 * exhausts. Measured on this board: 29 x "unable to alloc pbuf in recv_handler"
 * in 8 seconds, from a main loop that merely blocked on inbyte(). A callback
 * that blocks for a minute is the same bug with a longer window.
 *
 * `ERR busy` is the single exception, and it is deliberate: a bounded
 * udp_sendto() with no flash access and no waiting. It exists because the
 * pipeline pumps the stack while it runs, so callbacks keep firing during a long
 * UPDATE -- which means a busy board can answer instead of going silent, and
 * silence is the one response an operator cannot tell from a dead board.
 *
 *
 * WHY THE PARSER REFUSES ARGUMENTS
 * ════════════════════════════════════════════════════════════════════════════
 *
 * No command may carry a server address, a port or a filename. The TFTP server
 * stays compiled in, so nothing on the wire can steer where an image comes from.
 * That is the entire security argument for opening this port at all, given the
 * content path is already unauthenticated (see the ICD § Security posture), and
 * it is enforced here rather than left as a rule to remember -- same discipline
 * as the flash write guard, which is a range check inside the flash functions
 * rather than a note at their call sites.
 *
 * A verb followed by any non-whitespace byte is an error, not an argument.
 *
 *
 * ON pbufs AND WHAT COMES OFF THE WIRE
 * ════════════════════════════════════════════════════════════════════════════
 *
 * The callback owns the pbuf and every path out of it frees exactly once. The
 * payload may be chained, so it is read with pbuf_copy_partial() rather than
 * through p->payload -- a 64-byte command will arrive in one pbuf on this MAC
 * every single time, which is precisely what would let that bug survive the
 * bench.
 *
 * The request buffer is NUL-terminated here, by us, after the copy. Nothing from
 * the wire is trusted to terminate anything.
 */

#include <string.h>

#include "ctrl_channel.h"
#include "net_pump.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "xil_printf.h"

/* Long enough for the reply to leave the MAC before a deliberate reset. */
#define CTRL_DRAIN_MS   500U

/* Longest verb is "STATUS"; the +2 leaves room to notice an overlong token. */
#define CTRL_TOKEN_MAX    8U

static struct udp_pcb *ctrl_pcb;
static struct netif   *ctrl_netif;

/*
 * Who asked, and what for. Written by the callback, read by the main loop --
 * both run from net_pump() on the same thread of control, so `volatile`
 * documents the handoff rather than synchronising it (the same note as
 * tftp_xfer_t).
 */
static volatile ctrl_verb_t ctrl_pending = CTRL_VERB_NONE;
static volatile u8_t        ctrl_busy;      /* a verb is executing right now */
static volatile u8_t        ctrl_has_peer;  /* peer_* below are meaningful   */
static ip_addr_t            ctrl_peer_ip;
static u16_t                ctrl_peer_port;

/* ── Reply construction ───────────────────────────────────────────────────── */

const char *
ctrl_verb_str(ctrl_verb_t v)
{
	switch (v) {
	case CTRL_VERB_STATUS: return "STATUS";
	case CTRL_VERB_UPDATE: return "UPDATE";
	case CTRL_VERB_UPDATE_FORCE: return "UPDATE FORCE";
	case CTRL_VERB_SWITCH: return "SWITCH";
	case CTRL_VERB_GOLDEN: return "GOLDEN";
	default:               return "-";
	}
}

void
ctrl_reply_init(ctrl_reply_t *r)
{
	r->len = 0U;
	r->buf[0] = '\0';
}

void
ctrl_reply_str(ctrl_reply_t *r, const char *s)
{
	/* Leave room for the terminating NUL and the trailing newline the sender
	 * appends; truncate rather than overflow. */
	while (*s != '\0' && r->len < (CTRL_REPLY_MAX - 2U)) {
		r->buf[r->len++] = *s++;
	}
	r->buf[r->len] = '\0';
}

void
ctrl_reply_u32(ctrl_reply_t *r, u32 v)
{
	char  tmp[11];
	int   i = 0;

	if (v == 0U) {
		ctrl_reply_str(r, "0");
		return;
	}
	while (v != 0U && i < (int)sizeof(tmp)) {
		tmp[i++] = (char)('0' + (v % 10U));
		v /= 10U;
	}
	while (i-- > 0) {
		if (r->len < (CTRL_REPLY_MAX - 2U)) {
			r->buf[r->len++] = tmp[i];
		}
	}
	r->buf[r->len] = '\0';
}

void
ctrl_reply_hex32(ctrl_reply_t *r, u32 v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	ctrl_reply_str(r, "0x");
	for (i = 28; i >= 0; i -= 4) {
		if (r->len < (CTRL_REPLY_MAX - 2U)) {
			r->buf[r->len++] = hex[(v >> i) & 0xFU];
		}
	}
	r->buf[r->len] = '\0';
}

/* ── Sending ─────────────────────────────────────────────────────────────── */

/*
 * One line, one datagram, to an explicit address. The newline is appended here
 * so no caller can forget it and so `nc -u` prints the reply without waiting.
 */
static void
ctrl_send_to(const ip_addr_t *ip, u16_t port, const char *line, u16_t len)
{
	struct pbuf *p;

	if (ctrl_pcb == NULL) {
		return;
	}

	p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(len + 1U), PBUF_RAM);
	if (p == NULL) {
		return;   /* best effort; the verb still ran and the console still says so */
	}
	if (pbuf_take_at(p, line, len, 0U) != ERR_OK ||
	    pbuf_take_at(p, "\n", 1U, len) != ERR_OK) {
		pbuf_free(p);
		return;
	}
	(void)udp_sendto(ctrl_pcb, p, ip, port);
	pbuf_free(p);
}

void
ctrl_reply_send(const ctrl_reply_t *r)
{
	xil_printf("CTL: %s\r\n", r->buf);

	if (ctrl_has_peer) {
		ctrl_send_to(&ctrl_peer_ip, ctrl_peer_port, r->buf, r->len);
	}
}

void
ctrl_drain(void)
{
	if (ctrl_netif != NULL) {
		net_pump_ms(ctrl_netif, CTRL_DRAIN_MS);
	}
}

/* ── Parsing ─────────────────────────────────────────────────────────────── */

typedef enum {
	CTRL_PARSE_OK = 0,   /* *out is the verb, no arguments followed        */
	CTRL_PARSE_EMPTY,    /* nothing but whitespace                         */
	CTRL_PARSE_UNKNOWN,  /* a token, but not one of ours                   */
	CTRL_PARSE_ARGS      /* a known verb followed by something; *out valid */
} ctrl_parse_t;

static int
ctrl_is_space(char c)
{
	return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

/* Case-insensitive compare against an uppercase literal. */
static int
ctrl_token_is(const char *tok, const char *upper)
{
	while (*upper != '\0') {
		char c = *tok++;

		if (c >= 'a' && c <= 'z') {
			c = (char)(c - ('a' - 'A'));
		}
		if (c != *upper++) {
			return 0;
		}
	}
	return (*tok == '\0');
}

static ctrl_parse_t
ctrl_parse(const char *line, ctrl_verb_t *out)
{
	char tok[CTRL_TOKEN_MAX + 1U];
	u16_t n = 0U;

	*out = CTRL_VERB_NONE;

	while (*line != '\0' && ctrl_is_space(*line)) {
		line++;
	}
	if (*line == '\0') {
		return CTRL_PARSE_EMPTY;
	}

	while (*line != '\0' && !ctrl_is_space(*line)) {
		if (n < CTRL_TOKEN_MAX) {
			tok[n] = *line;
		}
		n++;
		line++;
	}
	if (n > CTRL_TOKEN_MAX) {
		return CTRL_PARSE_UNKNOWN;   /* too long to be any verb we have */
	}
	tok[n] = '\0';

	if      (ctrl_token_is(tok, "STATUS")) { *out = CTRL_VERB_STATUS; }
	else if (ctrl_token_is(tok, "UPDATE")) { *out = CTRL_VERB_UPDATE; }
	else if (ctrl_token_is(tok, "SWITCH")) { *out = CTRL_VERB_SWITCH; }
	else if (ctrl_token_is(tok, "GOLDEN")) { *out = CTRL_VERB_GOLDEN; }
	else                                   { return CTRL_PARSE_UNKNOWN; }

	/*
	 * Whatever follows must be whitespace, all the way to the end. A verb with
	 * an operand is refused even though we know the verb -- naming it in the
	 * error is help, not permission.
	 *
	 * EXACTLY ONE EXCEPTION, and it is written here rather than anywhere else so
	 * that the rule and its exception cannot drift apart. `UPDATE FORCE` admits
	 * the downgrade a bare UPDATE refuses. It is an
	 * operand rather than a fifth verb because it modifies UPDATE and is
    	 * meaningless without it, and because an operator who types it is saying
	 * something about THIS update rather than putting the board into a mode.
	 *
	 * The exception stays narrow by construction: the operand is only consulted
	 * for UPDATE, only the single literal FORCE is accepted, and anything else
	 * -- including FORCE after any other verb -- still lands on CTRL_PARSE_ARGS.
	 */
	while (*line != '\0' && ctrl_is_space(*line)) {
		line++;
	}

	if (*line != '\0') {
		char opd[CTRL_TOKEN_MAX + 1U];
		u16_t m = 0U;

		if (*out != CTRL_VERB_UPDATE) {
			return CTRL_PARSE_ARGS;
		}

		while (*line != '\0' && !ctrl_is_space(*line)) {
			if (m < CTRL_TOKEN_MAX) {
				opd[m] = *line;
			}
			m++;
			line++;
		}
		if (m > CTRL_TOKEN_MAX) {
			return CTRL_PARSE_ARGS;
		}
		opd[m] = '\0';

		if (!ctrl_token_is(opd, "FORCE")) {
			return CTRL_PARSE_ARGS;
		}
		*out = CTRL_VERB_UPDATE_FORCE;

		/* Nothing may follow the operand either. */
		while (*line != '\0') {
			if (!ctrl_is_space(*line)) {
				return CTRL_PARSE_ARGS;
			}
			line++;
		}
	}
	return CTRL_PARSE_OK;
}

/* ── Receive ─────────────────────────────────────────────────────────────── */

/* Build and send a one-line error to the requester of THIS packet -- never to
 * ctrl_peer_*, which may belong to a command already running. */
static void
ctrl_err_to(const ip_addr_t *ip, u16_t port, ctrl_verb_t v, const char *why)
{
	ctrl_reply_t r;

	ctrl_reply_init(&r);
	ctrl_reply_str(&r, "ERR ");
	ctrl_reply_str(&r, ctrl_verb_str(v));
	ctrl_reply_str(&r, " ");
	ctrl_reply_str(&r, why);

	xil_printf("CTL: %s\r\n", r.buf);
	ctrl_send_to(ip, port, r.buf, r.len);
}

static void
ctrl_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
             const ip_addr_t *addr, u16_t port)
{
	char         req[CTRL_REQ_MAX + 1U];
	ctrl_verb_t  verb;
	ctrl_parse_t pr;

	(void)arg;
	(void)pcb;

	if (p == NULL) {
		return;
	}

	/* Oversized junk gets nothing at all: no reply, no log line, no state
	 * change. That bounds what this port can be used to amplify. */
	if (p->tot_len > CTRL_REQ_MAX) {
		pbuf_free(p);
		return;
	}

	(void)pbuf_copy_partial(p, req, p->tot_len, 0U);
	req[p->tot_len] = '\0';
	pbuf_free(p);

	pr = ctrl_parse(req, &verb);

	switch (pr) {
	case CTRL_PARSE_EMPTY:
		ctrl_err_to(addr, port, CTRL_VERB_NONE, "empty");
		return;
	case CTRL_PARSE_UNKNOWN:
		/* The verb is deliberately not echoed back: an unknown token is
		 * attacker-controlled text and a reply must not be paddable by it. */
		ctrl_err_to(addr, port, CTRL_VERB_NONE, "unknown verb");
		return;
	case CTRL_PARSE_ARGS:
		ctrl_err_to(addr, port, verb, "no arguments accepted");
		return;
	default:
		break;
	}

	if (ctrl_busy || ctrl_pending != CTRL_VERB_NONE) {
		ctrl_err_to(addr, port, verb, "busy");
		return;
	}

	ip_addr_copy(ctrl_peer_ip, *addr);
	ctrl_peer_port = port;
	ctrl_has_peer  = 1U;
	ctrl_pending   = verb;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int
ctrl_open(struct netif *netif)
{
	ctrl_netif = netif;

	ctrl_pcb = udp_new();
	if (ctrl_pcb == NULL) {
		return 0;
	}
	if (udp_bind(ctrl_pcb, IP_ADDR_ANY, CTRL_PORT) != ERR_OK) {
		udp_remove(ctrl_pcb);
		ctrl_pcb = NULL;
		return 0;
	}
	udp_recv(ctrl_pcb, ctrl_recv_cb, NULL);
	return 1;
}

ctrl_verb_t
ctrl_take_pending(void)
{
	ctrl_verb_t v = ctrl_pending;

	if (v != CTRL_VERB_NONE) {
		ctrl_pending = CTRL_VERB_NONE;
		ctrl_busy    = 1U;
	}
	return v;
}

void
ctrl_begin_local(void)
{
	ctrl_busy     = 1U;
	ctrl_has_peer = 0U;
}

void
ctrl_done(void)
{
	ctrl_busy     = 0U;
	ctrl_has_peer = 0U;
}
