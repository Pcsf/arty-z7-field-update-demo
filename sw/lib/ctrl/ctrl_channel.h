/*
 * ctrl_channel.h — the UDP command channel, public contract.
 *
 * Contract: doc/icd_control_channel.md. The verbs, their side effects and the
 * error taxonomy live there; this header is the API and the wire constants.
 *
 * WHAT THIS MODULE OWNS AND WHAT IT DOES NOT
 *
 * It owns the wire: the PCB, the port, the parser, which requester is waiting,
 * and the reply line. It knows nothing about roles, flash, the updater pipeline
 * or MultiBoot, and it must stay that way — the application owns the actions.
 *
 * That split is what keeps the serial menu honest. Both the menu and the network
 * reach the same handlers in the application; this module is one of two ways in,
 * not a second implementation of anything.
 */

#ifndef CTRL_CHANNEL_H
#define CTRL_CHANNEL_H

#include "lwip/netif.h"
#include "xil_types.h"

/*
 * Fixed and compiled in. A configurable port would be an operand, and operands
 * are exactly what the ICD forbids this channel to carry.
 */
#define CTRL_PORT        6100U

/* Anything longer is dropped in silence — see the ICD's error contract. */
#define CTRL_REQ_MAX       64U

/* One line, one datagram, no chaining. */
#define CTRL_REPLY_MAX    200U

typedef enum {
	CTRL_VERB_NONE = 0,   /* nothing pending                                */
	CTRL_VERB_STATUS,
	CTRL_VERB_UPDATE,
	CTRL_VERB_UPDATE_FORCE,  /* `UPDATE FORCE` -- may install a downgrade    */
	CTRL_VERB_SWITCH,
	CTRL_VERB_GOLDEN
} ctrl_verb_t;

/* Uppercase name, for replies and the console. Never NULL. */
const char *ctrl_verb_str(ctrl_verb_t v);

/*
 * A reply under construction. Built a piece at a time rather than with
 * snprintf(): newlib's formatted output would be the largest thing in this
 * binary by a wide margin, and the image budget is measured (README § image
 * sizes). Overflow truncates rather than corrupting -- a truncated status line
 * is a bad reply, a smashed stack is a bad board.
 */
typedef struct {
	char  buf[CTRL_REPLY_MAX];
	u16_t len;
} ctrl_reply_t;

void ctrl_reply_init(ctrl_reply_t *r);
void ctrl_reply_str(ctrl_reply_t *r, const char *s);
void ctrl_reply_u32(ctrl_reply_t *r, u32 v);
void ctrl_reply_hex32(ctrl_reply_t *r, u32 v);   /* prints as 0x%08x */

/*
 * Bind the control port and start listening. `netif` is the interface the
 * module pumps when it has to drain a reply; it is not otherwise used.
 *
 * Returns 1 on success, 0 if the PCB could not be allocated or bound — in which
 * case the board simply has no control channel and everything else still works.
 * Say so on the console; do not treat it as fatal.
 */
int ctrl_open(struct netif *netif);

/*
 * Main loop: take the pending verb, if any, and mark the channel busy.
 *
 * Returns CTRL_VERB_NONE when there is nothing to do. From the moment this
 * returns a verb until ctrl_done(), any further command is answered `ERR busy`
 * by the receive callback rather than queued -- a queued command is a command
 * that runs at a time nobody chose.
 */
ctrl_verb_t ctrl_take_pending(void);

/*
 * Main loop: a verb is about to run that came from the serial menu rather than
 * the wire. Marks the channel busy so a command arriving mid-way is answered
 * `ERR busy` instead of racing the flash, and leaves no requester -- replies
 * from a local verb go to the console only.
 *
 * The menu and the wire reach the same handlers; this is the only difference
 * between the two paths.
 */
void ctrl_begin_local(void);

/* Main loop: the verb has finished. Accepts commands again. */
void ctrl_done(void);

/*
 * Send `r` to whoever sent the verb currently being executed, and echo it to
 * the console. Safe to call when the verb came from the serial menu instead --
 * there is no requester, so it only echoes.
 *
 * Best effort by design: a send that fails is not retried and does not change
 * what the verb did. UDP was chosen knowing that.
 */
void ctrl_reply_send(const ctrl_reply_t *r);

/*
 * Pump the stack long enough for a just-sent reply to leave the MAC.
 *
 * For the two verbs that reset the board. Without it the operator's `nc` times
 * out on a command that worked perfectly, which is the most misleading failure
 * this channel could produce.
 */
void ctrl_drain(void);

#endif /* CTRL_CHANNEL_H */
