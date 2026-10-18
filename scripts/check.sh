#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compile-test the module without installing it: W=1 build, sparse (if
# installed) and checkpatch --strict.
#
#   scripts/check.sh                      # against the running kernel
#   KDIR=~/src/linux-6.18 scripts/check.sh
#
# KDIR must be a configured kernel tree (at least `make modules_prepare`).
# Without a Module.symvers (no full kernel build) modpost cannot resolve
# symbols; that is reported as a warning instead of failing the check.

set -eu

cd "$(dirname "$0")/.."

KVER=${KVER:-$(uname -r)}
KDIR=${KDIR:-/lib/modules/$KVER/build}

if [ ! -f "$KDIR/Makefile" ]; then
	echo "check.sh: no kernel tree at $KDIR (set KDIR=)" >&2
	exit 2
fi

modpost_warn=
if [ ! -f "$KDIR/Module.symvers" ]; then
	echo "check.sh: $KDIR/Module.symvers missing, unresolved symbols are warnings only"
	modpost_warn=KBUILD_MODPOST_WARN=1
fi

sparse_arg=
if command -v sparse >/dev/null 2>&1; then
	sparse_arg=C=2
else
	echo "check.sh: sparse not installed, skipping"
fi

status=0
log=$(mktemp)
trap 'rm -f "$log"; make -s KDIR="$KDIR" clean >/dev/null 2>&1 || :' EXIT

echo "== build (W=1${sparse_arg:+, sparse}) against $(make -s -C "$KDIR" kernelversion 2>/dev/null || echo "$KDIR")"
# shellcheck disable=SC2086
make KDIR="$KDIR" W=1 $sparse_arg $modpost_warn >"$log" 2>&1 || :
grep -E 'warning:|error:|ERROR:' "$log" || :
if [ ! -f xiaomi-book14.ko ]; then
	echo "FAIL: module did not build" >&2
	tail -20 "$log" >&2
	status=1
elif grep -q 'xiaomi-book14\.c:[0-9]*:[0-9]*: \(warning\|error\)' "$log"; then
	echo "FAIL: compiler/sparse diagnostics above" >&2
	status=1
else
	echo "ok"
fi

echo "== checkpatch --strict"
checkpatch="$KDIR/scripts/checkpatch.pl"
if [ -x "$checkpatch" ]; then
	# LINUX_VERSION_CODE: intentional out-of-tree minimum-version guard
	"$checkpatch" --no-tree --strict --quiet --show-types \
		--ignore LINUX_VERSION_CODE,CONSTANT_COMPARISON \
		-f xiaomi-book14.c && echo "ok" || status=1
else
	echo "check.sh: $checkpatch not found, skipping"
fi

exit $status
