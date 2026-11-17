#!/usr/bin/env bash
# Host side helper: download the GSI, build, push to the phone, run.
set -euo pipefail

cd "$(dirname "$0")"
source ./config.env

DL=downloads
DEV=/data/local/tmp/hollowroot

ADB=(adb)
[ -n "$ADB_SERIAL" ] && ADB=(adb -s "$ADB_SERIAL")

su_run() { "${ADB[@]}" shell "su -c '$*'"; }
die() { echo "error: $*" >&2; exit 1; }

usage() {
    cat <<USAGE
usage: ./hollowroot.sh <command>

  fetch          download and unpack the GSI from GSI_URL (config.env)
  build          compile loader, guardian and helpers with the NDK
  push           copy binaries, scripts and system.img to the phone
  smoke          run the environment checks, does not stop host services
  start [secs]   full session, default 120 seconds. Needs --i-have-a-backup
  takeover [secs]  display stack only (SurfaceFlinger + bootanimation)
  logs           print the last session log
  clean          detach loop devices left over after a crash

start and takeover stop the host graphical stack for the session.
Hold Volume Up + Volume Down for 2 seconds to leave early.
USAGE
}

find_img() {
    if [ -n "$GSI_IMG" ] && [ -f "$GSI_IMG" ]; then echo "$GSI_IMG"; return; fi
    find "$DL" -maxdepth 2 -name '*.img' -size +500M 2>/dev/null | head -n1
}

cmd_fetch() {
    if [ -n "$(find_img)" ]; then echo "image already there: $(find_img)"; return; fi
    [ -n "$GSI_URL" ] || die "set GSI_URL in config.env"
    mkdir -p "$DL"
    local name=${GSI_URL##*/}
    [ "$name" = download ] && name=$(basename "$(dirname "$GSI_URL")")
    name=${name%%\?*}
    local file="$DL/$name"
    if [ ! -f "$file" ]; then
        curl -L --fail --progress-bar -o "$file.part" "$GSI_URL"
        mv "$file.part" "$file"
    fi
    if [ -n "$GSI_SHA256" ]; then
        echo "$GSI_SHA256  $file" | sha256sum -c - || die "checksum mismatch, delete $file and retry"
    fi
    case "$file" in
        *.7z|*.zip|*.xz|*.tar*) 7z x -y -o"$DL" "$file" >/dev/null ;;
        *.img) ;;
        *) die "unknown archive type: $file" ;;
    esac
    # tar.xz needs a second pass
    for t in "$DL"/*.tar; do [ -f "$t" ] && tar -C "$DL" -xf "$t"; done
    [ -n "$(find_img)" ] || die "no .img found after unpacking"
    echo "image: $(find_img)"
}

cmd_build() {
    local ndk=${NDK:-${ANDROID_NDK_HOME:-}}
    [ -n "$ndk" ] || die "set NDK in config.env or ANDROID_NDK_HOME"
    make NDK="$ndk"
}

cmd_push() {
    [ -x build/loader ] || die "run ./hollowroot.sh build first"
    local img; img=$(find_img)
    [ -n "$img" ] || die "no GSI image, run ./hollowroot.sh fetch"
    su_run "mkdir -p $DEV"
    "${ADB[@]}" push build/loader build/guardian scripts/run_session.sh /data/local/tmp/
    su_run "mv /data/local/tmp/loader /data/local/tmp/guardian /data/local/tmp/run_session.sh $DEV/ && chmod 755 $DEV/loader $DEV/guardian $DEV/run_session.sh"
    local want have
    want=$(stat -c %s "$img")
    have=$("${ADB[@]}" shell "su -c 'stat -c %s $DEV/system.img 2>/dev/null || echo 0'" | tr -d '\r')
    if [ "$want" = "$have" ]; then
        echo "system.img already on the phone (same size), skipping"
    else
        "${ADB[@]}" push "$img" /data/local/tmp/system.img.part
        su_run "mv /data/local/tmp/system.img.part $DEV/system.img"
    fi
}

cmd_smoke() {
    "${ADB[@]}" shell "su -c 'sh $DEV/run_session.sh smoke'"
}

cmd_start() {
    local mode=$1; shift
    local secs=120 ok=0
    for a in "$@"; do
        case "$a" in
            --i-have-a-backup) ok=1 ;;
            [0-9]*) secs=$a ;;
            *) die "unknown argument: $a" ;;
        esac
    done
    [ "$ok" = 1 ] || die "add --i-have-a-backup once /sdcard is copied off the phone and the copy is verified"
    echo "starting $mode session for ${secs}s, screen will switch to the guest"
    "${ADB[@]}" shell "su -c 'HOLLOWROOT_BACKUP_OK=1 nohup sh $DEV/run_session.sh $secs $mode >/dev/null 2>&1 &'"
    echo "follow with: ./hollowroot.sh logs"
}

cmd_logs() { su_run "cat $DEV/session.log"; }

cmd_clean() {
    su_run "for d in /dev/block/loop45 /dev/block/loop47; do b=\$(cat /sys/block/\${d##*/}/loop/backing_file 2>/dev/null); case \"\$b\" in $DEV/*|/dev/hollowroot/*) losetup -d \$d && echo detached \$d;; esac; done; rm -rf /dev/hollowroot"
}

case "${1:-}" in
    fetch) cmd_fetch ;;
    build) cmd_build ;;
    push) cmd_push ;;
    smoke) cmd_smoke ;;
    start) shift; cmd_start --framework "$@" ;;
    takeover) shift; cmd_start --takeover "$@" ;;
    logs) cmd_logs ;;
    clean) cmd_clean ;;
    *) usage; exit 1 ;;
esac
