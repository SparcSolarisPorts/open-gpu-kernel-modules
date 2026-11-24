#!/bin/sh
#
# List the symbols a kernel module needs that neither the module itself nor
# the kernel modules it depends on provide.  modload(1M) reports only the
# first such symbol; this prints all of them, so they can be fixed in one pass.
#
# Two kinds of problem are reported:
#   - no kernel module exports the symbol: it has to be written or stubbed;
#   - only a module outside the dependency list exports it: the module needs
#     another -N<module> at link time, and nothing else.
#
# Read-only.
#
#   solaris-undef.sh module.kmod [dependency ...]
#
# Dependencies are written as they are on the link line without -N, for
# example misc/pcie.  The default is the list nvidia.kmod links with.
# unix and genunix are always available.

mod=$1
if [ -z "$mod" ] || [ ! -f "$mod" ]; then
    echo "usage: $0 module.kmod [misc/dependency ...]" >&2
    exit 2
fi
shift
if [ $# -gt 0 ]; then
    deps="$*"
else
    deps="misc/pcie misc/sha2 misc/kcf"
fi

TMP=${TMPDIR:-/tmp}/solaris-undef.$$
mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' 0
trap 'exit 1' 1 2 15

# An undefined symbol has no value, so its line may be "U name" (two fields)
# or "<value> U name"; a defined global one is "<value> <TYPE> <name>" with an
# upper-case type letter.  The type is the next-to-last field.
UNDEF='NF >= 2 && $(NF-1) == "U" { print $NF }'
DEFINED='NF >= 3 && $(NF-1) ~ /^[A-TV-Z]$/ { print $NF, m }'

# Symbols the module needs.
nm -p "$mod" | awk "$UNDEF" | sort -u > "$TMP/need"

# Who provides what: "symbol module" lines.  The module provides its own.
nm -p "$mod" | awk -v m=self "$DEFINED" > "$TMP/prov"

for f in $(find /platform/sun4v/kernel /kernel /usr/kernel -type f \
        \( -name unix -o -name genunix -o -path '*/misc/sparcv9/*' \) \
        2>/dev/null); do
    case $f in
        */misc/sparcv9/*) name=misc/$(basename "$f") ;;
        *)                name=core ;;
    esac
    nm -p "$f" 2>/dev/null | awk -v m="$name" "$DEFINED" >> "$TMP/prov"
done

awk -v deps="$deps" '
    FNR == NR {
        prov[$1] = ($1 in prov) ? prov[$1] " " $2 : $2
        next
    }
    {
        n++
        if (!($1 in prov)) { none[++nn] = $1; next }
        c = split(prov[$1], mods, " ")
        ok = 0
        for (i = 1; i <= c; i++)
            if (mods[i] == "core" || mods[i] == "self" ||
                index(" " deps " ", " " mods[i] " ") > 0)
                ok = 1
        if (ok) { good++; next }
        dep[++nd] = $1 "  (" mods[1] ")"
    }
    END {
        printf "needed:      %d\n", n
        printf "resolved:    %d\n", good
        printf "no module provides (write or stub these): %d\n", nn
        for (i = 1; i <= nn; i++) printf "    %s\n", none[i]
        printf "provided only by a module not in the dependency list (%s): %d\n",
            deps, nd
        for (i = 1; i <= nd; i++) printf "    %s\n", dep[i]
    }
' "$TMP/prov" "$TMP/need"

# The search covers unix, genunix and misc modules only.  Some kernel routines
# live elsewhere (bcopy, bzero, copyin and copyout are undefined in unix and
# genunix on the T8-1 and still resolve), so a name listed above is a lead, not
# a verdict: modload is the real test.
echo "note: symbols can be provided by kernel files this script does not search;"
echo "      a successful modload is the proof that all of them resolve."
