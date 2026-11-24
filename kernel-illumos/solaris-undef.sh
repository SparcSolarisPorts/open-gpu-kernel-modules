#!/bin/sh
#
# List the symbols that a kernel module needs and that neither the module
# itself nor the running kernel provides.  modload(1M) reports only the first
# such symbol; this prints all of them, so they can be fixed in one pass.
#
# Read-only.  Usage: solaris-undef.sh _out/SunOS_sparcv9/nvidia.kmod
#
# The kernel's symbols come from unix, genunix and every misc module, which
# is where DDI routines such as ddi_regs_map_setup() and kobj_open_file() live.
# Solaris nm -p prints "value type name"; type U is undefined.

mod=$1
if [ -z "$mod" ] || [ ! -f "$mod" ]; then
    echo "usage: $0 module.kmod" >&2
    exit 2
fi

TMP=${TMPDIR:-/tmp}/solaris-undef.$$
mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' 0
trap 'exit 1' 1 2 15

# An undefined symbol has no value, so its line may be "U name" (two fields)
# or "<blanks> U name"; a defined one is "<value> <type> <name>".  The type is
# the next-to-last field and is a single letter.
UNDEF='NF >= 2 && $(NF-1) == "U" { print $NF }'
DEFINED='NF >= 3 && $(NF-1) ~ /^[A-Za-z]$/ && $(NF-1) != "U" { print $NF }'

# Names the module needs.
nm -p "$mod" | awk "$UNDEF" | sort -u > "$TMP/need"

# Names the module defines itself, and the kernel's.
{
    nm -p "$mod" | awk "$DEFINED"

    for f in $(find /platform/sun4v/kernel /kernel -type f \
            \( -name unix -o -name genunix -o -path '*/misc/sparcv9/*' \) \
            2>/dev/null); do
        nm -p "$f" 2>/dev/null | awk "$DEFINED"
    done
} | sort -u > "$TMP/have"

echo "needed:      $(wc -l < "$TMP/need" | tr -d ' ')"
echo "provided:    $(wc -l < "$TMP/have" | tr -d ' ') (module plus kernel)"
echo "unresolved:"
comm -23 "$TMP/need" "$TMP/have" | sed 's/^/    /'
