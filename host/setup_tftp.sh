#!/bin/bash
# setup_tftp.sh — stand up a TFTP server on this host for the `tftp_client` example.
#
# Serves /srv/tftp, which is the same root host/release.sh already writes its
# boot images into, so the updater needs no second server.
#
# Run it once:  ./host/setup_tftp.sh                serve on wlan0 (LAN mode)
#           or:  BENCH_MODE=direct ./host/setup_tftp.sh   the point-to-point link
#           or:  ./host/setup_tftp.sh 10.0.0.1          an explicit address
# It asks for your password (installing a package and writing /srv need root)
# and is safe to re-run — every step is idempotent.
#
#
# Arch / Omarchy. On another distro the package and unit names differ; the
# probe files and the self-test at the end are still what you want.

set -euo pipefail

TFTP_ROOT="/srv/tftp"
PKG="tftp-hpa"

log()  { echo "[tftp-setup] $*"; }
fail() { echo "[tftp-setup] ERROR: $*" >&2; exit 1; }

# ── 1. The server ────────────────────────────────────────────────────────────
if command -v in.tftpd >/dev/null 2>&1; then
    log "in.tftpd already present"
else
    log "installing $PKG (needs root)"
    sudo pacman -S --needed --noconfirm "$PKG"
fi

command -v in.tftpd >/dev/null 2>&1 || fail "in.tftpd still not on PATH after install"

# ── 2. The root ──────────────────────────────────────────────────────────────
log "creating $TFTP_ROOT"
sudo install -d -m 0755 "$TFTP_ROOT"

# ── 3. The probe files ───────────────────────────────────────────────────────
# Every line is exactly 32 bytes including its newline, so block boundaries land
# on line boundaries and a mis-sequenced or duplicated block is visible by eye
# rather than by hexdump. 512 / 32 = 16 lines per TFTP block.
#
#   probe.txt      24 lines =  768 bytes → 512 + 256   (short final block)
#   probe_1024.txt 32 lines = 1024 bytes → 512 + 512 + 0
#
# The second one is the edge case worth having on hand: a file that is an exact
# multiple of the block size ends with a DATA packet carrying ZERO bytes, and a
# client that treats "payload < 512" as the only terminator without handling
# len == 0 will hang there. Fetch probe.txt first, then that one.
gen_probe() {
    local lines="$1" out="$2" i text
    : > "$out"
    for ((i = 1; i <= lines; i++)); do
        case $i in
            1)  text="L$(printf '%02d' $i) TFTP probe file" ;;
            16) text="L$(printf '%02d' $i) <<< END BLOCK 1" ;;
            17) text="L$(printf '%02d' $i) <<< START BLOCK 2" ;;
            32) text="L$(printf '%02d' $i) <<< END BLOCK 2" ;;
            *)  text="L$(printf '%02d' $i) ..............." ;;
        esac
        printf '%-31.31s\n' "$text" >> "$out"
    done
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

gen_probe 24 "$tmp/probe.txt"
gen_probe 32 "$tmp/probe_1024.txt"

# Belt and braces: the whole point of these files is their exact size.
[[ $(stat -c %s "$tmp/probe.txt")      -eq 768  ]] || fail "probe.txt is not 768 bytes"
[[ $(stat -c %s "$tmp/probe_1024.txt") -eq 1024 ]] || fail "probe_1024.txt is not 1024 bytes"

sudo install -m 0644 "$tmp/probe.txt"      "$TFTP_ROOT/probe.txt"
sudo install -m 0644 "$tmp/probe_1024.txt" "$TFTP_ROOT/probe_1024.txt"
log "wrote $TFTP_ROOT/probe.txt (768 B) and probe_1024.txt (1024 B)"

# ── 4. The service ───────────────────────────────────────────────────────────
# tftp-hpa ships both a socket unit and a service unit; which one is right
# depends on the packaging, so ask systemd rather than assume.
if systemctl list-unit-files 2>/dev/null | grep -q '^tftpd\.socket'; then
    UNIT=tftpd.socket
elif systemctl list-unit-files 2>/dev/null | grep -q '^tftpd\.service'; then
    UNIT=tftpd.service
else
    fail "no tftpd.socket or tftpd.service unit found — check how $PKG packaged it"
fi

log "enabling $UNIT"
sudo systemctl enable --now "$UNIT"
systemctl is-active --quiet "$UNIT" || fail "$UNIT did not come up: systemctl status $UNIT"
log "$UNIT active"

# Show the effective arguments — the served root lives here, and if it is not
# /srv/tftp the board will get "File not found" with everything else correct.
if [[ -f /etc/conf.d/tftpd ]]; then
    log "server args: $(grep -v '^\s*#' /etc/conf.d/tftpd | tr -s '\n' ' ')"
fi

# ── 5. Local self-test ───────────────────────────────────────────────────────
# Fetch through the network stack rather than checking the file exists in
# /srv/tftp — that would prove nothing about whether the server serves it.
#
# But be honest about what this does NOT prove. Addressing the host by its own
# IP routes over loopback, and every firewall trusts `lo`, so this passing tells
# you the server and its root are correct and tells you NOTHING about whether a
# machine on the LAN can reach it. Learned the hard way on 2026-08-13: this
# reported OK while the host firewall dropped every packet the board sent, and
# the board reported a timeout indistinguishable from "server down".

# Which interface the board is on decides which of the host's addresses to
# serve. Both bench layouts are supported and the board's own choice lives in
# TFTP_BENCH_DIRECT in examples/tftp_client/main.c — the two must
# agree, so the mode is named here rather than guessed.
#
#   BENCH_MODE=lan     board on the house LAN, host serves from wlan0  (default)
#   BENCH_MODE=direct  board cabled to the host, serves from the bench link
BENCH_MODE="${BENCH_MODE:-lan}"
BENCH_IF="${BENCH_IF:-enp196s0}"
LAN_IF="${LAN_IF:-wlan0}"

pick_ip() { ip -4 -brief addr show "$1" scope global 2>/dev/null \
            | awk '{print $3}' | cut -d/ -f1 | head -1; }

if [[ -n "${1:-}" ]]; then
    HOST_IP="$1"
    log "server address given explicitly: $HOST_IP"
elif [[ "$BENCH_MODE" == "direct" ]]; then
    HOST_IP="$(pick_ip "$BENCH_IF")"
    [[ -n "$HOST_IP" ]] || fail "BENCH_MODE=direct but $BENCH_IF has no IPv4 —
                                is host/10-fpga-bench.network installed?"
    log "direct mode: serving on $BENCH_IF ($HOST_IP)"
else
    HOST_IP="$(pick_ip "$LAN_IF")"
    [[ -n "$HOST_IP" ]] || fail "BENCH_MODE=lan but $LAN_IF has no IPv4"
    log "LAN mode: serving on $LAN_IF ($HOST_IP)"
    log "note: TFTP_BENCH_DIRECT in main.c must be 0 to match, and the board"
    log "      must be plugged into the router rather than into this host."
fi

[[ -n "$HOST_IP" ]] || fail "no usable IPv4 address on this host"
LAN_SUBNET="$(echo "$HOST_IP" | cut -d. -f1-3).0/24"

# The board's own address, for the control-channel firewall rule below. Same
# default as ctrl.sh; override if the board is not at .10.
BOARD_IP="${BOARD_IP:-$(echo "$HOST_IP" | cut -d. -f1-3).10}"

log "local self-test (loopback): fetching probe.txt from $HOST_IP"
( cd "$tmp" && rm -f probe.txt && tftp "$HOST_IP" -c get probe.txt )
[[ $(stat -c %s "$tmp/probe.txt") -eq 768 ]] || fail "fetched probe.txt is the wrong size"
log "local self-test OK — server and root are correct (says nothing about the LAN)"

# ── 6. Firewall check — report only ──────────────────────────────────────────
# The step that actually decides whether the board can talk to us. A default
# INPUT policy of DROP silently swallows the RRQ; the board sees no reply and
# reports a timeout that looks exactly like "server down".
#
# This only ever REPORTS. Opening a port on your machine is your call to make
# deliberately, not a side effect of running a setup script.
#
# TWO rules are needed, and the second is the one that surprises people.
#
# UDP/69 is the obvious half: the board's RRQ is unsolicited inbound. TFTP's own
# DATA/ACK exchange needs nothing more, because it is continuous -- the flow is
# refreshed every few milliseconds and conntrack keeps it ESTABLISHED.
#
# The control channel is not continuous. ctrl.sh opens a CONNECTED socket, so
# the board answers to an EPHEMERAL host port rather than to 6100, and that
# return path exists only as a conntrack entry. UPDATE goes quiet for ~40 s
# while the board erases and programs 4.3 MB, which is longer than
# nf_conntrack_udp_timeout (30 s by default). The entry is evicted mid-update,
# the board's 'OK UPDATE installed' arrives as unsolicited inbound, and ufw
# drops it -- so ctrl.sh reports 'no result within 120s' for an update that in
# fact succeeded. Allowing UDP from the board's address covers the ephemeral
# reply port, which is why this rule is by SOURCE and not by destination port.
if [[ "$(grep -c '^ENABLED=yes' /etc/ufw/ufw.conf 2>/dev/null || echo 0)" != "0" ]]; then
    policy="$(grep -E '^DEFAULT_INPUT_POLICY' /etc/default/ufw 2>/dev/null | cut -d'"' -f2)"
    log "ufw is ENABLED, default INPUT policy ${policy:-unknown}"
    echo
    log "  If the board times out, this is the first suspect. To allow it, run:"
    log "      sudo ufw allow from $LAN_SUBNET to any port 69 proto udp"
    log "  Scoped to the LAN rather than the world: a TFTP server is a file"
    log "  server, and --secure limits the path, not who may ask."
    echo
    log "  And for the control channel, scoped to the board alone:"
    log "      sudo ufw allow from ${BOARD_IP} proto udp"
    log "  Without it UPDATE still SUCCEEDS on the board, but its result reply is"
    log "  dropped and ctrl.sh reports 'no result within 120s'. The reply comes"
    log "  back to an ephemeral port after ~40 s of silence, by which time the"
    log "  conntrack entry has expired -- hence by source, not by port."
    echo
else
    log "ufw does not appear enabled; if the board times out, check"
    log "  sudo nft list ruleset   /   sudo iptables -L -n"
fi

log "Server is up. The board should use:  $HOST_IP"
log "If TFTP_SRV_* in examples/tftp_client/main.c is not $HOST_IP, fix it there."
echo
log "The remaining honest gap: nothing here proves a packet from the board's own"
log "address reached this host. If it still times out with the port open, watch"
log "for the RRQ arriving —"
log "      sudo tcpdump -ni any udp port 69"
log "and if nothing appears, the problem is upstream of this machine (the board"
log "is on a wired segment, this host is on wlan0 — they must actually bridge)."
