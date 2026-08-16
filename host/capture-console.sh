#!/bin/bash
# Console capture that survives the board losing power.
#
# A plain `cat /dev/ttyUSBn` dies at EOF the moment the FT2232H re-enumerates,
# and the log simply stops growing -- which reads exactly like a board that
# never came back. F5 cuts power on purpose, so a single-fd capture would miss
# the very boot it exists to record.
#
# Two defences: match on the durable by-id path (if01) rather than a ttyUSBn
# number that moves, and reopen in a loop so a disconnect costs a gap rather
# than the rest of the session. Each reopen is announced in the log, so a gap
# is visible as a gap instead of being silently stitched over.

LOG="${1:?usage: capture-reopen.sh <logfile>}"
STAMP='{ printf "%s %s\n", strftime("%H:%M:%S"), $0; fflush() }'

while true; do
    PORT=$(readlink -f /dev/serial/by-id/usb-Digilent_*-if01-port0 2>/dev/null)
    if [ -n "$PORT" ] && [ -c "$PORT" ]; then
        echo "$(date +%H:%M:%S) === capture attached to $PORT ===" >> "$LOG"
        stty -F "$PORT" 115200 cs8 -cstopb -parenb raw -echo 2>/dev/null
        cat "$PORT" 2>/dev/null | stdbuf -oL awk "$STAMP" >> "$LOG"
        echo "$(date +%H:%M:%S) === capture detached (port went away) ===" >> "$LOG"
    fi
    sleep 0.5
done
