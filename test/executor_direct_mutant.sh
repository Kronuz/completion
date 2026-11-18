#!/bin/sh
# The A2 mutant: the executor's Sync and SyncData call the File directly, as before the seam.
# usage: executor_direct_mutant.sh BASE_REF BUILD_DIR   (run from the checkout, built at the fix)
# Exits 127 when the mutant does not build, which never counts as a failing test; otherwise the
# test's own status. The fix's operation.h is restored on exit.
base=$1
build=$2
trap 'git checkout -- include/completion/operation.h' EXIT
git show "$base:include/completion/operation.h" > include/completion/operation.h || exit 127
cmake --build "$build" -j4 --target completion_sync_seam_test || exit 127
echo "mutant built: executor at $base"
"$build/completion_sync_seam_test" "$build/mutant-state"
