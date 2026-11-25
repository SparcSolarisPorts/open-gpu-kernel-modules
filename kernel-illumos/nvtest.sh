#!/bin/ksh
#
# nvtest.sh: trial-load the NVIDIA modules from /tmp so that a reboot puts
# the machine back the way it was.
#
#   nvtest.sh load [-d outdir] [-t minutes] [-a] [-m] [-n] [-p]
#   nvtest.sh keep
#   nvtest.sh revert [-r]
#   nvtest.sh status
#   nvtest.sh bind [-d outdir]
#
# load    Copies the built modules and .conf files to /tmp/nvtest and
#         lofs-mounts each one over its normal /kernel/drv path, so the
#         kernel finds it there and nothing from the build is written to
#         the root filesystem. Then it arms a watchdog that reverts and
#         reboots after -t minutes (default 10) unless you run "keep", and
#         registers the drivers with add_drv. By default only "nvidia" is
#         loaded; -a adds nvidia_modeset, nvidia_i2c and nvidia_uvm, -m
#         adds only nvidia_modeset (the one Solaris/sparcv9 builds).
#         With -n the driver is registered without the GPU's PCI IDs, so
#         it loads and runs its init code but is not attached to any GPU.
#         With -p the staged nvidia.conf also gets NVreg_GpuInitOnProbe=0,
#         so a later attach maps the BARs and identifies the chip but does
#         not start the GPU (no GSP boot, no firmware file needed). The
#         nvidia.conf in the source tree is not changed.
# bind    After "load -n", adds the PCI IDs with update_drv so the driver
#         attaches to the GPU. This is the step that touches the hardware.
# keep    Cancels the watchdog. The modules stay until the next reboot.
# revert  Removes the drivers, unmounts and deletes the staged files.
#         With -r it reboots afterwards. Safe to run again after a reset.
# status  Shows what is active.
#
# What survives a reboot: add_drv edits /etc/name_to_major, driver_aliases,
# driver_classes and minor_perm, and those edits stay. The lofs mounts and
# /tmp do not, so after a hard reset the drivers point at empty placeholder
# files and cannot load. Run "revert" once the machine is back. Copies of
# the four /etc files from before the trial are in /var/tmp/nvtest.
#
# The watchdog cannot help if the kernel hangs hard. Keep console, BMC or
# power access available for the first runs.
#
# The kernel directory (amd64 or sparcv9) and the default output directory
# (_out/SunOS_<isa>) come from isainfo.

set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin

STAGE=/tmp/nvtest
STATE=/var/tmp/nvtest
HERE=$(cd "$(dirname "$0")" && pwd)
SELF=$HERE/$(basename "$0")
ALL_MODS="nvidia nvidia_modeset nvidia_i2c nvidia_uvm"

die() { echo "nvtest: $*" >&2; exit 1; }
warn() { echo "nvtest: warning: $*" >&2; }
usage() { sed -n '6,10p' "$SELF" | sed 's/^# \{0,1\}//' >&2; exit 2; }
need_root() { [ "$(id -u)" = 0 ] || die "run as root"; }

[ "$(uname -s)" = SunOS ] || die "this script is for SunOS only"

KISA=$(isainfo -k)
NISA=$(isainfo -n)
case $NISA in
amd64) TAG=x86_64 ;;
*)     TAG=$NISA ;;
esac

confsrc() {
    case $1 in
    nvidia)         echo "$HERE/nvidia/nvidia.conf" ;;
    nvidia_modeset) echo "$HERE/nvidia-modeset/nvidia_modeset.conf" ;;
    nvidia_i2c)     echo "$HERE/nvidia-i2c/nvidia_i2c.conf" ;;
    nvidia_uvm)     echo "$HERE/nvidia-uvm/nvidia_uvm.conf" ;;
    esac
}

# Undo whatever a trial did. Safe to call with no trial active, and safe to
# call twice.
undo() {
    mods=$(cat "$STATE/mods" 2>/dev/null)
    rev=
    for m in $mods; do rev="$m $rev"; done

    # Only drivers that load registered are in name_to_major, because load
    # refuses to start when one is already there.
    for m in $rev; do
        if grep "^$m " /etc/name_to_major >/dev/null 2>&1; then
            rem_drv "$m" || warn "rem_drv $m failed; reboot, then run revert again"
        fi
    done

    clean=1
    if [ -f "$STATE/mounts" ]; then
        while read -r dst; do
            if mount | grep "^$dst " >/dev/null 2>&1; then
                umount "$dst" || { warn "cannot unmount $dst"; clean=0; }
            fi
        done < "$STATE/mounts"
    fi

    if [ -f "$STATE/placeholders" ] && [ $clean = 1 ]; then
        while read -r p; do
            [ -s "$p" ] || rm -f "$p"
        done < "$STATE/placeholders"
    fi

    if [ $clean = 1 ]; then
        rm -rf "$STAGE"
        rm -f "$STATE/active" "$STATE/mods" "$STATE/mounts" \
            "$STATE/placeholders" "$STATE/keep"
    else
        warn "mounts remain, so staged files and state were kept; reboot, then run revert again"
    fi
    return $clean
}

fail() { undo; die "$*"; }

stop_watchdog() {
    if [ -f "$STATE/watchdog.pid" ]; then
        kill "$(cat "$STATE/watchdog.pid")" 2>/dev/null
        rm -f "$STATE/watchdog.pid"
    fi
}

cmd_load() {
    need_root
    mods=nvidia
    mins=10
    out=$HERE/_out/SunOS_$TAG
    noalias=0
    noinit=0
    while getopts d:t:amnp o; do
        case $o in
        d) out=$OPTARG ;;
        t) mins=$OPTARG ;;
        a) mods=$ALL_MODS ;;
        m) mods="nvidia nvidia_modeset" ;;
        n) noalias=1 ;;
        p) noinit=1 ;;
        *) usage ;;
        esac
    done
    case $mins in
    ''|*[!0-9]*) die "-t takes a whole number of minutes" ;;
    esac
    [ "$mins" -gt 0 ] || die "-t must be at least 1"

    mkdir -p "$STATE" || die "cannot create $STATE"
    [ -f "$STATE/active" ] && die "a trial is already active; run keep or revert"
    [ -d "/kernel/drv/$KISA" ] || die "/kernel/drv/$KISA does not exist"

    for m in $mods; do
        [ -f "$out/$m.kmod" ] || die "missing $out/$m.kmod (build with gmake modules)"
        [ -f "$(confsrc "$m")" ] || die "missing $(confsrc "$m")"
        if grep "^$m " /etc/name_to_major >/dev/null 2>&1; then
            die "$m is already registered; refusing to touch an existing install"
        fi
    done
    [ -f "$out/nvidia.aliases" ] || die "missing $out/nvidia.aliases"
    [ -d /kernel/firmware/nvidia ] || warn "/kernel/firmware/nvidia not found; GSP firmware is needed to initialize a GPU"

    bk=$STATE/backup.$(date +%Y%m%d%H%M%S)
    mkdir -p "$bk" || die "cannot create $bk"
    for f in name_to_major driver_aliases driver_classes minor_perm; do
        cp -p "/etc/$f" "$bk/$f" || die "cannot back up /etc/$f"
    done

    rm -rf "$STAGE"
    mkdir -p "$STAGE" || die "cannot create $STAGE"
    : > "$STATE/mounts"
    : > "$STATE/placeholders"
    echo "$mods" > "$STATE/mods"
    date > "$STATE/active"
    rm -f "$STATE/keep"

    for m in $mods; do
        cp "$out/$m.kmod" "$STAGE/$m" || fail "copy of $m failed"
        cp "$(confsrc "$m")" "$STAGE/$m.conf" || fail "copy of $m.conf failed"
        if [ $noinit = 1 ] && [ "$m" = nvidia ]; then
            echo 'NVreg_GpuInitOnProbe=0;' >> "$STAGE/$m.conf" ||
                fail "cannot edit the staged $m.conf"
        fi
        for pair in "$STAGE/$m:/kernel/drv/$KISA/$m" "$STAGE/$m.conf:/kernel/drv/$m.conf"; do
            src=${pair%%:*}
            dst=${pair#*:}
            if [ ! -e "$dst" ]; then
                : > "$dst" || fail "cannot create $dst"
                echo "$dst" >> "$STATE/placeholders"
            fi
            mount -F lofs "$src" "$dst" || fail "lofs mount of $dst failed"
            echo "$dst" >> "$STATE/mounts"
        done
    done

    # Arm the watchdog before anything can hang.
    NVTEST_WATCHDOG=1 nohup ksh -c "sleep $(expr "$mins" \* 60); [ -f '$STATE/keep' ] || '$SELF' revert -r" \
        >/dev/null 2>&1 &
    echo $! > "$STATE/watchdog.pid"

    aliases=$(tr '\n' ' ' < "$out/nvidia.aliases")
    for m in $mods; do
        case $m in
        nvidia)
            if [ $noalias = 1 ]; then
                add_drv -m '* 0666 root sys' nvidia
            else
                add_drv -m '* 0666 root sys' -i "$aliases" nvidia
            fi ;;
        nvidia_i2c) add_drv nvidia_i2c ;;
        *)          add_drv -m '* 0666 root sys' "$m" ;;
        esac || fail "add_drv $m failed; trial removed"
    done

    [ $noinit = 1 ] && echo "nvtest: GPU init at attach is off (NVreg_GpuInitOnProbe=0)"
    if [ $noalias = 1 ]; then
        echo "nvtest: loaded without PCI IDs: $mods (no GPU is attached)"
        echo "nvtest: check it with: modinfo | grep nvidia"
        echo "nvtest: to attach the GPU run: $SELF bind"
    else
        echo "nvtest: loaded: $mods"
    fi
    echo "nvtest: pre-trial /etc copies: $bk"
    echo "nvtest: the machine reverts and reboots in $mins minutes unless you run:"
    echo "        $SELF keep"
}

cmd_keep() {
    need_root
    [ -f "$STATE/active" ] || die "no trial is active"
    : > "$STATE/keep"
    stop_watchdog
    echo "nvtest: watchdog cancelled. The modules stay until the next reboot."
    echo "nvtest: after a reboot, run '$SELF revert' to clear the /etc entries."
}

cmd_revert() {
    need_root
    reboot_after=0
    while getopts r o; do
        case $o in
        r) reboot_after=1 ;;
        *) usage ;;
        esac
    done
    [ "${NVTEST_WATCHDOG:-}" = 1 ] || stop_watchdog
    undo
    rc=$?
    rm -f "$STATE/watchdog.pid"
    if [ $reboot_after = 1 ]; then
        echo "nvtest: rebooting"
        exec reboot
    fi
    exit $((1 - rc))
}

cmd_bind() {
    need_root
    out=$HERE/_out/SunOS_$TAG
    while getopts d: o; do
        case $o in
        d) out=$OPTARG ;;
        *) usage ;;
        esac
    done
    [ -f "$STATE/active" ] || die "no trial is active; run load -n first"
    grep "^nvidia " /etc/name_to_major >/dev/null 2>&1 || die "nvidia is not registered"
    [ -f "$out/nvidia.aliases" ] || die "missing $out/nvidia.aliases"

    aliases=$(tr '\n' ' ' < "$out/nvidia.aliases")
    update_drv -a -i "$aliases" nvidia || die "update_drv failed; run revert"
    echo "nvtest: PCI IDs added; the driver attaches to the GPU now"
}

cmd_status() {
    if [ -f "$STATE/active" ]; then
        echo "trial active since: $(cat "$STATE/active")"
        echo "modules: $(cat "$STATE/mods" 2>/dev/null)"
        if [ -f "$STATE/watchdog.pid" ] && kill -0 "$(cat "$STATE/watchdog.pid")" 2>/dev/null; then
            echo "watchdog: armed"
        elif [ -f "$STATE/keep" ]; then
            echo "watchdog: cancelled (keep)"
        else
            echo "watchdog: not running"
        fi
        mount | grep "^/kernel/drv" || echo "mounts: none (after a reboot?)"
        modinfo | grep nvidia || echo "loaded modules: none"
    else
        echo "no trial active"
    fi
}

cmd=${1:-}
[ $# -gt 0 ] && shift
case $cmd in
load)   cmd_load "$@" ;;
keep)   cmd_keep ;;
revert) cmd_revert "$@" ;;
bind)   cmd_bind "$@" ;;
status) cmd_status ;;
*)      usage ;;
esac
