#!/bin/sh
# Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
# SPDX-License-Identifier: MIT
#
# Mutant: PosixIO's shared directory lock takes LOCK_EX. Builds owner_lock_test on a mutated copy
# of the sources and exits 0 only when it fails at the matrix's reader-beside-reader pairing; 2
# when the anchor is not found once, 3 when the build fails, 4 when the test passes or fails
# elsewhere. The checkout itself is never modified.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
work="$root/build-mutant-reader"
anchor='do { result = flock_(fd, LOCK_SH | LOCK_NB); }'
count=$(grep -cF "$anchor" "$root/include/completion/posix.h")
[ "$count" = 1 ] || { echo "mutant: anchor occurs $count times"; exit 2; }

rm -rf "$work"
mkdir -p "$work/src"
cp -R "$root/CMakeLists.txt" "$root/include" "$root/test" "$root/examples" "$root/bench" "$root/tools" "$work/src/"
sed 's/flock_(fd, LOCK_SH | LOCK_NB)/flock_(fd, LOCK_EX | LOCK_NB)/' "$root/include/completion/posix.h" > "$work/src/include/completion/posix.h"
grep -qF 'flock_(fd, LOCK_SH' "$work/src/include/completion/posix.h" && { echo "mutant: not applied"; exit 2; }

cmake -S "$work/src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCOMPLETION_ASIO=OFF >/dev/null &&
	cmake --build "$work/build" --target completion_owner_lock_test -j4 >/dev/null || { echo "mutant: build failed"; exit 3; }

"$work/build/completion_owner_lock_test" "$work/state" >"$work/out.txt" 2>&1
status=$?
tail -5 "$work/out.txt"
expected='reader then reader: busy, expected acquired'
if [ "$status" -eq 1 ] && grep -qF "$expected" "$work/out.txt"; then
	echo "mutant: killed by the matrix ($expected)"
	exit 0
fi
echo "mutant: survived or failed elsewhere (exit $status)"
exit 4
