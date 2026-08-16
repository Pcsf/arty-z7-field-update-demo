#!/bin/bash
# ctrl.sh — send one verb to the board's control channel and print the reply.
#
# Contract: doc/icd_control_channel.md. Plain ASCII over UDP, so this script is
# a convenience and not a dependency — anything that can send a datagram works.
#
# Usage: ./host/ctrl.sh <STATUS|UPDATE [FORCE]|SWITCH|GOLDEN> [board-ip]
#
#   STATUS   role, slot, update_present, boot_attempts, running VERSION
#   UPDATE   fetch, verify, flash, verify, commit — about a minute. Does NOT
#            change what boots; send SWITCH afterwards for that.
#   UPDATE FORCE
#            same pipeline, but installs a manifest whose version is OLDER than
#            the one recorded as installed, which a bare UPDATE refuses. It
#            relaxes that policy and NOTHING else: hash, length, boot header and
#            read-back all still apply, and an image whose digest already matches
#            the slot is still skipped as a no-op.
#   SWITCH   clear boot_attempts and reboot into the update slot
#   GOLDEN   condemn the installed update and reboot into golden
#
# UPDATE answers twice: 'OK UPDATE started' immediately, then the result when
# the pipeline finishes. This waits for the second one and exits as soon as it
# lands, rather than burning the whole budget.
#
# SWITCH and GOLDEN reset the board on purpose: the reply arrives, then the link
# goes down for a few seconds. That is success, not a failure to answer.
#
# NO EXTERNAL DEPENDENCY, DELIBERATELY. The first version of this script used
# nc and did not survive contact with the bench — nc is not installed on this
# host and the failure surfaced with the board already booted and waiting.
# bash's own /dev/udp redirection is always there, and one connected socket per
# run also means the kernel drops anything not from the board.
#
# AND WHY THE REPLY IS READ WITH dd, NOT WITH read.
#
# bash's `read` builtin reads a byte at a time. On a STREAM that is merely
# slow; on a DATAGRAM socket it is wrong, because a read() shorter than the
# datagram consumes the whole datagram and discards the remainder. So `read`
# returns the first character of the reply and throws the rest away, then
# blocks forever waiting for a newline that was already dropped.
#
# Measured on this bench, 2026-08-15: against a local UDP responder sending
# "hello-from-listener", `read -t 3` timed out holding exactly 'h'. The board
# was answering correctly the whole time — the console had already printed the
# reply and socat received it — so this looked exactly like a board that would
# not answer. One read() per datagram is the fix; `dd bs=4096 count=1` is one
# read() and needs nothing installed.

set -euo pipefail

# Flags are stripped BEFORE the positional arguments are read. Doing it the
# other way round is how "./ctrl.sh SWITCH --no-wait" ended up trying to reach a
# board called "--no-wait" -- the flag was consumed as the IP, the socket open
# failed, and the verb was never sent at all. It cost an F7 run.
WAIT_FOR_REBOOT=1
POSITIONAL=()
for a in "$@"; do
    case "$a" in
        --no-wait) WAIT_FOR_REBOOT=0 ;;
        -*)        echo "[ctrl] unknown option: $a" >&2; exit 2 ;;
        *)         POSITIONAL+=("$a") ;;
    esac
done

VERB="${POSITIONAL[0]:-STATUS}"
VERB="${VERB^^}"

# FORCE is an OPERAND of UPDATE, not a verb and not a flag. It has to be read
# before board-ip or it is consumed as one -- the same trap the flag stripping
# above exists to avoid, and no board is ever called FORCE.
OPERAND=""
if [ "${POSITIONAL[1]:-}" != "" ] && [ "${POSITIONAL[1]^^}" = "FORCE" ]; then
    OPERAND="FORCE"
    POSITIONAL=("${POSITIONAL[0]}" "${POSITIONAL[@]:2}")
    if [ "$VERB" != "UPDATE" ]; then
        echo "[ctrl] FORCE is only accepted after UPDATE" >&2; exit 2
    fi
fi
[ -n "$OPERAND" ] && VERB="$VERB $OPERAND"

BOARD_IP="${POSITIONAL[1]:-192.168.1.10}"
PORT=6100

# Every verb answers immediately. Only UPDATE has a second, slow reply.
ACK_WAIT=2
RESULT_WAIT=120

echo "[ctrl] -> ${BOARD_IP}:${PORT}  ${VERB}"

# A connected UDP socket: bash sends to the board and reads only what comes
# back from it. Datagram boundaries are line boundaries here because every
# reply is exactly one line, by contract.
exec 3<>"/dev/udp/${BOARD_IP}/${PORT}" || {
    echo "[ctrl] cannot open a UDP socket to ${BOARD_IP}:${PORT}" >&2
    exit 1
}
printf '%s\n' "$VERB" >&3

# THE FIRST COMMAND AFTER A BOOT CAN BE LOST, AND ONLY STATUS MAY BE RETRIED.
#
# Measured 2026-08-15: four STATUS sent to a freshly booted board produced three
# replies, and the board's console showed only three -- the missing one never
# arrived, dropped inbound while the link was still settling. The TFTP client
# already carries a 30-retry budget for exactly this (TFTP_RRQ_MAX_RETRIES), and
# for the same reason.
#
# Retrying is safe for STATUS, which is read-only by contract, and NOT safe for
# the other three: a lost *reply* is indistinguishable from a lost *request*, so
# a retry can run a verb the board already ran. GOLDEN and SWITCH happen to be
# idempotent in effect, but UPDATE costs a minute and a slot rewrite, and
# guessing which is which at 2 a.m. is not a contract. One shot; confirm with
# STATUS.
if [ "$VERB" = "STATUS" ]; then
    STATUS_TRIES=3
else
    STATUS_TRIES=1
fi

# One datagram per call: a single read() of a buffer larger than any reply the
# ICD allows (200 bytes). Prints nothing and returns 1 on timeout.
recv_one() {
    local out
    out=$(timeout "$1" dd bs=4096 count=1 status=none <&3) || return 1
    [ -n "$out" ] || return 1
    printf '%s' "${out%$'\n'}" | tr -d '\r'
}

got=0

# First reply, with retries where retrying is safe.
try=1
while :; do
    if line=$(recv_one "$ACK_WAIT"); then
        echo "$line"
        got=1
        break
    fi
    [ "$try" -ge "$STATUS_TRIES" ] && break
    try=$((try + 1))
    echo "[ctrl] no reply, retry $try/$STATUS_TRIES (link may still be settling)" >&2
    printf '%s\n' "$VERB" >&3
done

# 'OK UPDATE started' is an acknowledgement, not the answer. The pipeline sends
# the result about a minute later, on the same socket.
case "$line" in
    "OK UPDATE started"*)
        if line=$(recv_one "$RESULT_WAIT"); then
            echo "$line"
        else
            echo "[ctrl] no result within ${RESULT_WAIT}s - send STATUS to see where it got to" >&2
        fi
        ;;
esac

exec 3<&-

# ── After a resetting verb: narrate the wait, then prove the board is back ────
#
# The board goes quiet for ~15 s after SWITCH and ~5 s after GOLDEN, and the
# silence is the confusing part — it looks identical to a command that failed.
# What follows is NOT a progress bar: the expected sequence is printed because
# it is what the firmware does by contract, and the two transitions that are
# actually OBSERVED are marked as such.
#
# Why the two differ: the update role proves the PL heartbeat is still moving
# for a fixed 10 s BEFORE it brings the network up, so a board that just
# switched is deliberately unreachable while it decides whether to commit.
# Golden has no such probe, so it is back as soon as the link negotiates.
reboot_wait() {
    local what="$1" deadline=90 down=0 line="" hinted=0 t=0

    # Wall clock, not an iteration count. Each pass through this loop costs
    # between one and three seconds -- `ping -W1` blocks for a second when the
    # board is down, and the UDP probe up to two more -- so counting passes
    # reported roughly half the real elapsed time and made a 17 s reboot look
    # like 8 s. SECONDS is a bash builtin and needs nothing.
    SECONDS=0

    echo
    echo "[ctrl] the board is resetting — the link will drop now."
    if [ "$what" = "SWITCH" ]; then
        echo "[ctrl] expected, update role — measured ~15 s from the link dropping:"
        echo "[ctrl]    ~4 s   FSBL loads the 4 MB bitstream from QSPI"
        echo "[ctrl]   +10 s   payload health probe — heartbeat must keep moving"
        echo "[ctrl]           (the network is deliberately DOWN for this part)"
        echo "[ctrl]    ~3 s   link negotiation, then the control port binds"
    else
        echo "[ctrl] expected, golden role — measured ~5 s from the link dropping:"
        echo "[ctrl]    ~4 s   FSBL loads the 4 MB bitstream from QSPI"
        echo "[ctrl]           (no health probe in golden — it is the safe state)"
        echo "[ctrl]    ~3 s   link negotiation, then the control port binds"
    fi
    echo

    while [ "$SECONDS" -lt "$deadline" ]; do
        t=$SECONDS
        if ping -c1 -W1 "$BOARD_IP" >/dev/null 2>&1; then
            alive=1
        else
            alive=0
        fi

        # PHASE 1 -- the board must be seen to GO DOWN before anything it says
        # can be believed. Without this the loop "confirms" the board is back
        # within a second of the verb, because the pre-reset instance is still
        # answering: it replies, drains the reply, and only then resets.
        if [ "$down" -eq 0 ]; then
            if [ "$alive" -eq 0 ]; then
                down=1
                echo "[ctrl] t+${t}s  OBSERVED: link down — the board is rebooting"
            fi
        elif [ "$alive" -eq 1 ]; then
            # PHASE 2 -- back on the network. Only an OK STATUS counts as
            # healthy: an ERR ... busy means a verb is still executing, which
            # is a board that is up but not yet ready to be asked anything.
            exec 3<>"/dev/udp/${BOARD_IP}/${PORT}" 2>/dev/null || true
            printf 'STATUS\n' >&3 2>/dev/null || true
            line=$(timeout 2 dd bs=4096 count=1 status=none <&3 2>/dev/null) || line=""
            exec 3<&- 2>/dev/null || true
            line="$(printf '%s' "${line%$'\n'}" | tr -d '\r')"

            case "$line" in
                "OK STATUS"*)
                    echo "[ctrl] t+${t}s  OBSERVED: control channel answering — link healthy"
                    echo
                    echo "$line"
                    return 0
                    ;;
                ERR*)
                    echo "[ctrl] t+${t}s  up, but busy — a verb is still running"
                    ;;
            esac
        fi

        # One reassurance line, once, while the health probe is running and the
        # board genuinely cannot answer -- so the quiet stretch is explained
        # rather than endured. Only fires if we are still waiting by then.
        if [ "$hinted" -eq 0 ] && [ "$down" -eq 1 ] && [ "$SECONDS" -ge 6 ] \
           && [ "$what" = "SWITCH" ]; then
            hinted=1
            echo "[ctrl] t+${SECONDS}s  still quiet — this is the 10 s health probe, not a failure"
        fi
        sleep 1
    done

    if [ "$down" -eq 0 ]; then
        echo "[ctrl] the board never went down — the reset may not have happened" >&2
    else
        echo "[ctrl] board did not come back within ${deadline}s" >&2
    fi
    echo "[ctrl] check the serial console, then send STATUS. A board that fails" >&2
    echo "[ctrl] its health check stays down on purpose until the watchdog" >&2
    echo "[ctrl] returns it to golden." >&2
    return 1
}

case "$VERB" in
    SWITCH|GOLDEN)
        if [ "$got" -eq 1 ] && [ "$WAIT_FOR_REBOOT" -eq 1 ] \
           && [ "${line#OK}" != "$line" ]; then
            reboot_wait "$VERB"
            exit $?
        fi
        ;;
esac

if [ "$got" -eq 0 ]; then
    echo "[ctrl] no reply within ${ACK_WAIT}s" >&2
    echo "[ctrl] check: link up? board at ${BOARD_IP}? JP4 in QSPI?" >&2
    exit 1
fi
