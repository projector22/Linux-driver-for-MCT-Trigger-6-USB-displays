#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Gareth Palmer
#
# End-to-end test of mct_t6: the real driver in a User-Mode Linux kernel,
# talking over dummy_hcd to a software Trigger 6 (t6emu, via raw-gadget),
# driven by a KMS client (t6drm), checked by verify.py.  Runs unprivileged.
#
# usage: tests/e2e/run.sh [--patch FILE]... [-j N]
#
#   --patch FILE  apply FILE (a -p1 diff against src/) to the scratch copy of
#                 the driver before building; src/ itself is never modified
#   -j N          parallel build jobs (default: nproc)
#
# Environment:
#   T6_E2E_WORK   work directory for downloads, builds and output
#                 (default: ${TMPDIR:-/tmp}/mct_t6-e2e)
#   T6_BUILDENV   optional script to source before building the kernel
#                 (e.g. to put flex/bison/libelf from a private prefix on
#                 PATH)
#   T6_PYTHON     python with Pillow and numpy (default: python3)
#
# Results: $T6_E2E_WORK/out (console.txt, dmesg.txt, init.log, and fence/,
# nofence/, lossless/ and always/ with frames, logs and expected images).
# Exit status is non-zero if anything failed.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${T6_E2E_WORK:-${TMPDIR:-/tmp}/mct_t6-e2e}
PYTHON=${T6_PYTHON:-python3}
JOBS=$(nproc)
PATCHES=()

KVER=7.2.10
KURL=https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-$KVER.tar.xz
KSHA256=2feaa70453e91f541be6a5c9078d8168e081af45a3d9718054850538cff2ebf3

while [ $# -gt 0 ]; do
	case $1 in
	--patch) PATCHES+=("$(realpath "$2")"); shift 2 ;;
	-j) JOBS=$2; shift 2 ;;
	-h|--help) sed -n '3,25p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

KSRC=$WORK/src/linux-$KVER
KBUILD=$WORK/build/uml
MOD=$WORK/mod
BIN=$WORK/bin
OUT=$WORK/out

step() { echo; echo "=== $*"; }

mkdir -p "$WORK/dl" "$WORK/src" "$KBUILD" "$BIN"

# --- 1. Kernel source: verified tarball, extracted once, never written ---
step "kernel source ($KVER)"
TARBALL=$WORK/dl/linux-$KVER.tar.xz
if [ ! -f "$TARBALL" ]; then
	curl -fL --retry 3 -o "$TARBALL.part" "$KURL"
	mv "$TARBALL.part" "$TARBALL"
fi
echo "$KSHA256  $TARBALL" | sha256sum -c --quiet -
if [ ! -f "$KSRC/.e2e-extracted" ]; then
	rm -rf "${KSRC:?}"
	tar -C "$WORK/src" -xf "$TARBALL"
	touch "$KSRC/.e2e-extracted"
fi

# --- 2. Configure and build the UML kernel out of tree -------------------
if [ -n "${T6_BUILDENV:-}" ]; then
	# shellcheck disable=SC1090
	source "$T6_BUILDENV"
fi
kmake() { make -C "$KSRC" O="$KBUILD" ARCH=um "$@"; }

step "configure UML kernel"
if [ ! -f "$KBUILD/.config" ] || [ "$HERE/uml.config" -nt "$KBUILD/.config" ]; then
	kmake -s defconfig
	"$KSRC/scripts/kconfig/merge_config.sh" -m -O "$KBUILD" \
		"$KBUILD/.config" "$HERE/uml.config" > /dev/null
	kmake -s olddefconfig
fi
# Every option in the fragment must have survived olddefconfig.
bad=0
while read -r line; do
	sym=$(sed -E 's/^# //; s/[= ].*//' <<< "$line")
	got=$(grep -E "^$sym=|^# $sym is not set" "$KBUILD/.config" || true)
	case $line in
	"# "*) [ -z "$got" ] || [ "$got" = "$line" ] || { echo "config: $line, got $got"; bad=1; } ;;
	*) [ "$got" = "$line" ] || { echo "config: want $line, got '${got}'"; bad=1; } ;;
	esac
done < <(grep -E '^(# )?CONFIG_' "$HERE/uml.config")
[ $bad -eq 0 ] || { echo "kernel config is missing required options" >&2; exit 1; }

step "build UML kernel"
kmake -j"$JOBS" -s

# --- 3. Driver and KUnit module, from a scratch copy of src/ -------------
step "build mct_t6.ko and mct_t6_test.ko"
rm -rf "${MOD:?}"
mkdir -p "$MOD"
cp -a "$REPO/src/." "$MOD/"
for p in "${PATCHES[@]}"; do
	echo "applying $p (to the scratch copy only)"
	patch -d "$MOD" -p2 --no-backup-if-mismatch < "$p"
done
make -C "$KBUILD" ARCH=um M="$MOD" CONFIG_MCT_T6_KUNIT_TEST=m -j"$JOBS" \
	modules 2>&1 | { grep -v -e 'Clock skew' -e 'modification time' || true; }
cp "$MOD/mct_t6.ko" "$MOD/mct_t6_test.ko" "$BIN/"

# --- 4. Emulator and client, static so the guest needs nothing else ------
step "build t6emu and t6drm"
CFLAGS="-O2 -g -Wall -Wextra -Wno-missing-field-initializers -Werror"
gcc $CFLAGS -static -pthread -o "$BIN/t6emu" "$HERE/t6emu.c"
gcc $CFLAGS -static -o "$BIN/t6drm" "$HERE/t6drm.c"

# --- 5. Boot UML with the host as root; uml-init.sh does the test --------
step "run in UML"
rm -rf "${OUT:?}"
mkdir -p "$OUT"
# UML backs guest RAM with a deleted file in TMPDIR, else /dev/shm (tmpfs).
set +e
env -u TMPDIR -u TMP -u TEMP timeout 1800 "$KBUILD/linux" \
	mem=1G ncpus=2 seccomp=on log_buf_len=16M \
	root=/dev/root rootfstype=hostfs rootflags=/ rw \
	con=null con0=null,fd:1 ssl=null \
	init="$HERE/uml-init.sh" T6_WORK="$BIN" T6_OUT="$OUT" \
	> "$OUT/console.txt" 2>&1 < /dev/null
uml=$?
set -e
grep '^init:' "$OUT/console.txt" || true
if [ $uml -ne 0 ]; then
	echo "UML exited with status $uml (124 = timed out)" >&2
	tail -50 "$OUT/console.txt" >&2
fi

# --- 6. Verify ------------------------------------------------------------
step "verify"
set +e
"$PYTHON" -I "$HERE/verify.py" "$OUT"
ver=$?
set -e
if [ $uml -ne 0 ] || [ $ver -ne 0 ]; then
	echo; echo "E2E FAILED (uml $uml, verify $ver); see $OUT" >&2
	exit 1
fi
echo; echo "E2E PASSED; output in $OUT"
