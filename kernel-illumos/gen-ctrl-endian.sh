#!/bin/sh
#
# Regenerate src/nvidia/src/kernel/gpu/gsp/kernel_gsp_ctrl_endian.h, the
# big-endian converters for RM control parameter structures.
#
#   kernel-illumos/gen-ctrl-endian.sh [report-file]
#
# Needs gcc (only to run the preprocessor) and python3. Run it on any host;
# the output does not depend on the host byte order.
#
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/.." && pwd)
CTRL=$R/src/common/sdk/nvidia/inc/ctrl
OUT=$R/src/nvidia/src/kernel/gpu/gsp/kernel_gsp_ctrl_endian.h
REPORT=${1:-}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/tu.c" <<'EOF'
#include <nvtypes.h>
#include <nvstatus.h>
#include <ctrl/ctrl0080.h>
#include <ctrl/ctrl2080.h>
#include <ctrl/ctrl0073.h>
#include <ctrl/ctrl90f1.h>
EOF

gcc -E -P \
    -I"$R/src/common/sdk/nvidia/inc" \
    -I"$R/src/common/sdk/nvidia/inc/hw" \
    -I"$R/src/nvidia/arch/nvalloc/common/inc" \
    -I"$R/src/common/inc" \
    "$TMP/tu.c" > "$TMP/tu.i"

set -- --pp "$TMP/tu.i" --out "$OUT" \
    --hdr "$CTRL"/ctrl2080/*.h "$CTRL"/ctrl0080/*.h "$CTRL"/ctrl0073/*.h "$CTRL"/ctrl90f1.h
[ -n "$REPORT" ] && set -- "$@" --report "$REPORT"

python3 -I "$HERE/gen-ctrl-endian.py" "$@"
