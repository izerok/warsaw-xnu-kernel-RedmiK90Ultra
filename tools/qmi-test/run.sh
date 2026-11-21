#!/bin/bash
# Build and run the tests of the QMI helpers (bsd/dev/redwood_qmi.c and bsd/dev/redwood_nas.c
# of the kernel tree) on this machine, under the address and undefined-behaviour sanitizers.
#
# With ath11k's sources at hand (tools/mk-ath11k.sh fetches them; ATH11K=<directory> to say where
# else they are) its own message tables are tested too; without, only what needs none of them.
# Nothing of ath11k is kept: its tables are taken out of qmi.c into the build directory each time.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=$ROOT/xnu-src/xnu-8796.121.2                 # the kernel tree being worked in, or else the copies that are in git
[ -f "$K/bsd/dev/redwood_qmi.c" ] || K=$ROOT/xnu/files
ATH=${ATH11K:-$ROOT/xnu-deps/build/ath11k/src/ath11k}
B=$ROOT/xnu-deps/build/qmi-test
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/redwood_qmi.h" "$K/bsd/sys/redwood_qrtr.h" "$K/bsd/sys/redwood_nas.h" "$K/bsd/sys/redwood_wds.h" "$K/bsd/sys/redwood_cell.h" "$K/bsd/sys/redwood_soc.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread"
if [ -f "$ATH/qmi.c" ]; then
	"$HERE/extract.py" "$ATH" > "$B/tables.inc"
	${CC:-cc} $FLAGS -I"$B/inc" -I"$B" -I"$HERE/shim" -I"$ATH" -o "$B/test" "$HERE/test_qmi.c" "$HERE/test_encode.c" "$HERE/test_tables.c" "$HERE/test_router.c" "$HERE/test_wcn.c" "$K/bsd/dev/redwood_qmi.c"
	echo "qmi-test: with ath11k's tables from $ATH ($(cat "$ATH/../.release" 2>/dev/null | cut -d' ' -f1))"
else
	echo "qmi-test: no ath11k sources in $ATH: only the tests that need none"
	${CC:-cc} $FLAGS -DNO_ATH11K -I"$B/inc" -o "$B/test" "$HERE/test_qmi.c" "$HERE/test_encode.c" "$HERE/test_tables.c" "$HERE/test_router.c" "$HERE/test_wcn.c" "$K/bsd/dev/redwood_qmi.c"
fi
"$B/test"
# The NAS test needs no ath11k tables; it does need the shim for <sys/systm.h> (printf, strings).
${CC:-cc} $FLAGS -I"$B/inc" -I"$HERE/shim" -o "$B/test-nas" "$HERE/test_nas.c" "$K/bsd/dev/redwood_nas.c"
"$B/test-nas"
# Same for the WDS session test (its fake transport drives DPM/WDA/WDS by hand).
${CC:-cc} $FLAGS -I"$B/inc" -I"$HERE/shim" -o "$B/test-wds" "$HERE/test_wds.c" "$K/bsd/dev/redwood_wds.c"
"$B/test-wds"
# Same for the cell test (fake data plane; the ARP/DHCP engine is pure frame code).
${CC:-cc} $FLAGS -I"$B/inc" -I"$HERE/shim" -o "$B/test-cell" "$HERE/test_cell.c" "$K/bsd/dev/redwood_cell.c"
"$B/test-cell"
# Same for the SoC test (fake boot-args through the shim for <pexpert/pexpert.h>).
${CC:-cc} $FLAGS -I"$B/inc" -I"$HERE/shim" -o "$B/test-soc" "$HERE/test_soc.c" "$K/bsd/dev/redwood_soc.c"
"$B/test-soc"
