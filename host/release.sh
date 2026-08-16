#!/bin/bash
# release.sh — Build boot images, compute SHA-256, copy to /srv/tftp
#
# Usage: ./release.sh [board]
#   board = arty (default)
#
# Prerequisites:
#   mkimage, bootgen in PATH
#   /srv/tftp writable

set -euo pipefail

TFTP_ROOT="/srv/tftp"
BOARD="${1:-arty}"

# /srv/tftp is root-owned, so publishing needs escalation — but only for the
# copy. Everything else runs as you, and if you are already root there is
# nothing to escalate.
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

log() { echo "[release] $*"; }

# ---- Arty Z7 ----
#
# PUBLISHES, it does not build. `make DESIGN=ps boot-image` builds the images
# inside the Vivado container, driven by BOOT_IMAGES in project.mk; this takes
# what that produced and makes it fetchable. Two steps, deliberately separate:
# building needs the toolchain, publishing needs root on /srv, and conflating
# them would mean running bootgen as root or the container as you.
#
# Only update.bin is published. golden.bin is written once over JTAG by
# `make flash-boot` and must never be reachable over the network — serving it
# would invite exactly the field overwrite that the golden-image discipline
# exists to prevent.
build_arty() {
    local img="build/boot/update.bin"

    log "Publishing Arty Z7 update image..."

    if [ ! -f "$img" ]; then
        log "  ERROR: no $img"
        log "  Build it first, inside the container:"
        log "    make DESIGN=ps boot-image"
        return 1
    fi

    # Manifest format 1 — doc/icd_updater.md § Manifest format. Key/value lines
    # in any order, with the format named on the first line so a board that does
    # not speak it refuses rather than half-reads it.
    #
    # The length is NOT redundant with the transfer size: it is what the updater
    # hashes over and what it rounds up to a sector boundary, and a mismatch
    # against the received count is how a truncated download gets caught.
    #
    # The version is asked of make rather than written here. project.mk declares
    # what each payload's VERSION register reads and update.bif names which
    # bitstream goes in the image; a copy of that number in this script would be
    # a second source of truth for the one value the board compares against.
    local sha len ver
    sha=$(sha256sum "$img" | awk '{print $1}')
    len=$(stat -c %s "$img")
    ver=$(make -s print-UPDATE_VERSION 2>/dev/null)

    case "$ver" in
        0x[0-9a-fA-F]*) ;;
        *)  log "  ERROR: could not read UPDATE_VERSION from make (got '$ver')"
            log "  The manifest must carry a version; refusing to publish one"
            log "  that the board would reject or, worse, misread."
            return 1 ;;
    esac

    printf 'manifest 1\nsha256 %s\nlength %s\nversion %s\n' \
           "$sha" "$len" "$ver" > /tmp/update.sha.$$

    $SUDO cp "$img" "$TFTP_ROOT/update.bin"
    $SUDO cp /tmp/update.sha.$$ "$TFTP_ROOT/update.sha"
    $SUDO chmod 0644 "$TFTP_ROOT/update.bin" "$TFTP_ROOT/update.sha"
    rm -f /tmp/update.sha.$$

    log "  update.bin  $len bytes"
    log "  sha256      $sha"
    log "  version     $ver"
    log "  published to $TFTP_ROOT"
}

# ---- Main ----
log "========================================="
log "Field Update Release — $(date)"
log "========================================="

case "$BOARD" in
    arty)   build_arty ;;
    *)      echo "Usage: $0 [arty]"; exit 1 ;;
esac

log "========================================="
log "Release complete"
log "========================================="
