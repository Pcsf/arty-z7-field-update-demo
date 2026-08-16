#!/bin/bash
# teardown_tftp.sh — take down the TFTP server that host/setup_tftp.sh brought up.
#
# The mirror of setup_tftp.sh, and deliberately its mirror in behaviour too: it
# stops what that script started, it VERIFIES the port is actually free rather
# than assuming the unit files did as they were told, and it only ever REPORTS
# on the firewall — removing a rule from your machine is your decision, not a
# side effect of running a teardown.
#
# Usage:
#   ./host/teardown_tftp.sh              stop and disable the server, keep /srv/tftp
#   ./host/teardown_tftp.sh --purge      also delete the probe files and /srv/tftp
#
# Asks for your password (stopping units and deleting under /srv need root).
# Safe to re-run: every step tolerates already being done.

set -euo pipefail

TFTP_ROOT="/srv/tftp"
PURGE=0

case "${1:-}" in
    --purge) PURGE=1 ;;
    "")      ;;
    *)       echo "usage: $0 [--purge]" >&2; exit 2 ;;
esac

log()  { echo "[tftp-teardown] $*"; }
fail() { echo "[tftp-teardown] ERROR: $*" >&2; exit 1; }

# ── 1. The units ─────────────────────────────────────────────────────────────
# Both are handled because which one is live depends on how it was started.
# setup_tftp.sh enables tftpd.socket, but tftp-hpa can also end up running as
# tftpd.service in standalone '--listen' mode, which holds port 69 itself and
# will happily keep serving after the socket unit is stopped. Stopping only the
# one you enabled is how a "stopped" server carries on answering.
for unit in tftpd.socket tftpd.service; do
    if systemctl list-unit-files 2>/dev/null | grep -q "^${unit}"; then
        if systemctl is-active --quiet "$unit" 2>/dev/null; then
            log "stopping $unit"
            sudo systemctl stop "$unit"
        else
            log "$unit already inactive"
        fi
        if systemctl is-enabled --quiet "$unit" 2>/dev/null; then
            log "disabling $unit"
            sudo systemctl disable "$unit"
        fi
    fi
done

# ── 2. Any stragglers ────────────────────────────────────────────────────────
# A standalone in.tftpd started by hand (rather than by systemd) survives every
# systemctl command above and is invisible unless you look for the process.
if pgrep -f "in.tftp[d]" >/dev/null 2>&1; then
    log "an in.tftpd process is still running — stopping it"
    sudo pkill -f "in.tftp[d]" || true
    sleep 1
fi

# ── 3. Verify, do not assume ─────────────────────────────────────────────────
# The point of the check: 'systemctl stop' succeeding says the unit stopped, not
# that nothing is listening on 69. Socket activation, a stray standalone daemon,
# or an entirely different tftpd would all survive a clean-looking stop.
if ss -lun 2>/dev/null | grep -qE "(^|[^0-9:]):69[[:space:]]"; then
    ss -lunp 2>/dev/null | grep ":69 " || ss -lun | grep ":69"
    fail "something is STILL listening on UDP/69 — see the line above"
fi
log "verified: nothing is listening on UDP/69"

# ── 4. The files ─────────────────────────────────────────────────────────────
if [[ "$PURGE" -eq 1 ]]; then
    if [[ -d "$TFTP_ROOT" ]]; then
        log "purging $TFTP_ROOT"
        sudo rm -f "$TFTP_ROOT/probe.txt" "$TFTP_ROOT/probe_1024.txt"
        # rmdir, not rm -rf: if anything else put files here — host/release.sh
        # stages boot images in this same directory — they are not ours to
        # delete, and the failure to remove a non-empty directory is the signal.
        if sudo rmdir "$TFTP_ROOT" 2>/dev/null; then
            log "removed $TFTP_ROOT"
        else
            log "kept $TFTP_ROOT — not empty (release.sh boot images live here too)"
            sudo ls -la "$TFTP_ROOT" | tail -n +2
        fi
    else
        log "$TFTP_ROOT does not exist"
    fi
else
    log "kept $TFTP_ROOT and the probe files (pass --purge to remove them)"
fi

# ── 5. Firewall — report only ────────────────────────────────────────────────
# Same policy as setup: this script never edits your firewall. The rule is
# harmless while it is scoped to the LAN, and you may well want it to survive
# for the next session rather than re-adding it every time.
if [[ "$(grep -c '^ENABLED=yes' /etc/ufw/ufw.conf 2>/dev/null || echo 0)" != "0" ]]; then
    if grep -q "dport 69" /etc/ufw/user.rules 2>/dev/null; then
        subnet="$(grep -m1 'dport 69' /etc/ufw/user.rules | grep -oE '\-s [0-9./]+' | cut -d' ' -f2)"
        echo
        log "ufw still allows UDP/69 from ${subnet:-the LAN}. Left in place on purpose."
        log "To remove it yourself:"
        log "      sudo ufw delete allow from ${subnet:-192.168.1.0/24} to any port 69 proto udp"
    fi
fi

echo
log "Server is down. Bring it back with:  ./host/setup_tftp.sh"
