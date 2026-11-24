#!/bin/sh
#
# get-gsp-firmware.sh: fetch the GSP firmware that the NVIDIA driver needs.
#
# The firmware ships inside NVIDIA's Linux driver package, not in this
# repository. This script downloads that package for the driver's version
# (read from ../version.mk), unpacks it, and installs the firmware files
# where nvidia_fw.c looks for them:
#
#   /kernel/firmware/nvidia/<version>/gsp_ga10x.bin  (and any other *.bin)
#
# Usage: get-gsp-firmware.sh [-v version] [-d destdir] [-f runfile]
#                            [-e extracted_dir] [-u url] [-c sha256] [-k]
#
#   -v  driver version (default: NVIDIA_VERSION from ../version.mk)
#   -d  install directory (default: /kernel/firmware/nvidia/<version>)
#   -f  use this already downloaded NVIDIA-Linux-x86_64-<version>.run
#   -e  use the firmware from this already extracted directory tree
#   -u  download URL (default: download.nvidia.com, standard file name)
#   -c  expected SHA-256 of the .run file
#   -k  keep the work directory
#
# The install step uses sudo when not run as root. The download and unpacked
# files need about 1 GB in $TMPDIR (default /var/tmp; /tmp on Solaris is RAM).

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
[ -d /usr/gnu/bin ] && PATH=/usr/gnu/bin:$PATH

die() { echo "get-gsp-firmware: $*" >&2; exit 1; }
say() { echo "get-gsp-firmware: $*"; }
usage() { sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

version=
dest=
run=
extracted=
url=
want_sum=
keep=0
while getopts v:d:f:e:u:c:k o; do
    case $o in
    v) version=$OPTARG ;;
    d) dest=$OPTARG ;;
    f) run=$OPTARG ;;
    e) extracted=$OPTARG ;;
    u) url=$OPTARG ;;
    c) want_sum=$OPTARG ;;
    k) keep=1 ;;
    *) usage ;;
    esac
done

if [ -z "$version" ]; then
    version=$(sed -n 's/^NVIDIA_VERSION *= *//p' "$HERE/../version.mk" 2>/dev/null | head -1)
fi
case $version in
''|*[!0-9.]*) die "cannot determine a driver version; pass -v" ;;
esac
[ -n "$dest" ] || dest=/kernel/firmware/nvidia/$version
[ -n "$url" ] || url=https://download.nvidia.com/XFree86/Linux-x86_64/$version/NVIDIA-Linux-x86_64-$version.run

# Hash of a file, whichever tool this system has.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{ print $1 }'
    elif command -v digest >/dev/null 2>&1; then
        digest -a sha256 "$1"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{ print $1 }'
    else
        return 1
    fi
}

fetch_url() {
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$2" "$1"
    else
        die "neither curl nor wget is installed; download the file elsewhere and use -f"
    fi
}

as_root() {
    if [ "$(id -u)" = 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        die "installing into $dest needs root, and sudo is not installed"
    fi
}

base=${TMPDIR:-/var/tmp}
work=$base/nvfw.$$
mkdir -p "$work" || die "cannot create $work"
cleanup() { [ $keep = 1 ] || rm -rf "$work"; }
trap cleanup 0
trap 'exit 1' 1 2 15

fwdir=

if [ -n "$extracted" ]; then
    [ -d "$extracted" ] || die "$extracted is not a directory"
    say "using the extracted tree $extracted"
else
    if [ -n "$run" ]; then
        [ -f "$run" ] || die "$run does not exist"
        case $run in /*) ;; *) run=$(pwd)/$run ;; esac
    else
        run=$work/NVIDIA-Linux-x86_64-$version.run
        say "downloading $url"
        fetch_url "$url" "$run" || die "download failed (is the version right? try -u or -f)"
    fi

    [ -s "$run" ] || die "$run is empty"
    case $(head -1 "$run" | cut -c1-2) in
    '#!') ;;
    *) die "$run does not look like an NVIDIA .run installer (no #! line)" ;;
    esac

    sum=$(sha256_of "$run") || sum=
    if [ -n "$want_sum" ]; then
        [ -n "$sum" ] || die "no sha256 tool found; cannot check -c"
        want=$(echo "$want_sum" | tr 'A-F' 'a-f')
        [ "$sum" = "$want" ] || die "checksum mismatch: got $sum, wanted $want"
        say "sha256 matches"
    elif [ -n "$sum" ]; then
        say "sha256 $sum (compare with NVIDIA's published value, or pass -c)"
    fi

    # The installer unpacks into ./NVIDIA-Linux-x86_64-<version> under the
    # current directory; it needs tar and xz or gzip, from /usr/gnu/bin if any.
    say "extracting (this takes a minute)"
    (cd "$work" && sh "$run" -x) >"$work/extract.log" 2>&1 || {
        tail -5 "$work/extract.log" >&2
        die "extraction failed. Extract it on another machine with: sh $run -x, then copy gsp_*.bin here and rerun with -e <dir>"
    }
    extracted=$work
fi

# The package layout is not fixed here: find the directory with gsp_ga10x.bin.
fw=$(find "$extracted" -type f -name 'gsp_ga10x.bin' 2>/dev/null | head -1)
if [ -z "$fw" ]; then
    echo "get-gsp-firmware: no gsp_ga10x.bin found under $extracted; .bin files there:" >&2
    find "$extracted" -type f -name '*.bin' 2>/dev/null | head -20 >&2
    die "firmware not found"
fi
fwdir=$(dirname "$fw")

say "firmware found in $fwdir"
as_root mkdir -p "$dest" || die "cannot create $dest"

count=0
for f in "$fwdir"/*.bin; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    as_root cp "$f" "$dest/$name" || die "copy of $name failed"
    as_root chmod 644 "$dest/$name" || die "chmod of $name failed"
    cmp -s "$f" "$dest/$name" || die "$dest/$name differs from the source after copying"
    count=$((count + 1))
done
[ $count -gt 0 ] || die "no .bin files in $fwdir"

say "installed $count file(s) into $dest:"
ls -l "$dest"

# The driver asks for these names (kernel-open/common/inc/nv-firmware.h).
# Only gsp_ga10x.bin is needed for the RTX A2000 Laptop (GA107).
for name in gsp_ga10x gsp_log_ga10x ucodes_ga10x gsp_tu10x; do
    [ -f "$dest/$name.bin" ] || say "note: $name.bin is not in the package"
done
