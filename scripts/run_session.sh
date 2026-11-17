#!/system/bin/sh
# Runs on the phone as root, normally started by hollowroot.sh.
#
#   run_session.sh smoke
#   run_session.sh <seconds> [--framework|--takeover]
#
# Sets up the two loop devices, starts the guardian (or the loader directly
# for smoke) and always tears everything down again on exit.

BASE=/data/local/tmp/hollowroot
IMG=$BASE/system.img
SYS_LOOP=/dev/block/loop45
FRP_LOOP=/dev/block/loop47
FRP_DIR=/dev/hollowroot
FRP_IMG=$FRP_DIR/frp.img
LOG=$BASE/session.log

die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
[ -f "$IMG" ] || die "$IMG not found, push the GSI image first"
[ -x "$BASE/loader" ] || die "$BASE/loader not found"

ARG=${1:-120}
if [ "$ARG" = smoke ]; then
    MODE=--smoke
else
    MODE=${2:---framework}
    case "$ARG" in ''|*[!0-9]*) die "seconds must be a number" ;; esac
    [ "$ARG" -ge 10 ] && [ "$ARG" -le 300 ] || die "seconds must be between 10 and 300"
    [ -x "$BASE/guardian" ] || die "$BASE/guardian not found"
    [ "$HOLLOWROOT_BACKUP_OK" = 1 ] || die "refusing to stop host services without a backup (see README)"
fi

backing() { cat "/sys/block/${1##*/}/loop/backing_file" 2>/dev/null; }

ATTACHED=""
ORIG_ENFORCE=$(getenforce)

cleanup() {
    for dev in $ATTACHED; do
        losetup -d "$dev" 2>/dev/null
    done
    rm -f "$FRP_IMG"
    rmdir "$FRP_DIR" 2>/dev/null
    [ "$ORIG_ENFORCE" = Enforcing ] && setenforce 1
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# Attach a loop device, but never take over one that is already in use.
attach() {
    dev=$1; file=$2; ro=$3
    [ -b "$dev" ] || die "$dev does not exist"
    cur=$(backing "$dev")
    [ -z "$cur" ] || die "$dev is already in use (backing file: $cur)"
    if [ "$ro" = ro ]; then
        losetup -r "$dev" "$file" || die "losetup $dev failed"
    else
        losetup "$dev" "$file" || die "losetup $dev failed"
    fi
    ATTACHED="$ATTACHED $dev"
    [ "$(backing "$dev")" = "$file" ] || die "$dev is not backed by $file"
}

# FRP stand-in lives in /dev (tmpfs), so nothing is written to flash.
mkdir -p "$FRP_DIR" || die "cannot create $FRP_DIR"
dd if=/dev/zero of="$FRP_IMG" bs=1024 count=512 2>/dev/null || die "cannot create $FRP_IMG"

chcon u:object_r:media_rw_data_file:s0 "$IMG"
attach "$SYS_LOOP" "$IMG" ro
attach "$FRP_LOOP" "$FRP_IMG" rw

# The kernel loop worker and the loader need this on OriginOS. Restored on exit.
setenforce 0

if [ "$MODE" = --smoke ]; then
    "$BASE/loader" "$SYS_LOOP" --smoke 2>&1 | tee "$LOG"
else
    echo "session start: $(date) mode=$MODE seconds=$ARG" > "$LOG"
    "$BASE/guardian" "$ARG" "$MODE" >> "$LOG" 2>&1
    echo "session end: $(date)" >> "$LOG"
fi
