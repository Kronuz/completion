#!/bin/sh
# Copyright © 2026 Germán Méndez Bravo (Kronuz)
# SPDX-License-Identifier: MIT
# One mutant of the File and IO members map_test covers: it breaks one PosixFile or PosixIO
# override in posix.h, rebuilds map_test, and runs it; the test must fail.
# usage: test/io_members_mutant.sh NAME [BUILD_DIR]   (from the checkout root)
# Exits 127 when the mutant does not apply or build, which never counts as a failing test;
# otherwise the test's own status. posix.h is restored on exit.
name=$1
build=${2:-build-mutant}
case $name in
preallocate) old='if (length && reserve(' new='if (false && reserve(' ;;
advise) old='if (fd_ < 0 || length == 0 || offset > limit) { return; }' new='return;' ;;
map_shared) old='PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)' new='PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)' ;;
link) old='::linkat(directory_, from.c_str(), directory_, to.c_str(), 0)' new='::renameat(directory_, from.c_str(), directory_, to.c_str())' ;;
extended_acl) old='inspection.extended_acl = extended_acl(fd_);' new='' ;;
create_mode) old='do { result = ::fchmod(fd, mode); } while (result < 0 && errno == EINTR);' new='result = 0;' ;;
*) echo "unknown mutant: $name" >&2; exit 127 ;;
esac
trap 'git checkout -- include/completion/posix.h' EXIT
python3 test/replace_once.py include/completion/posix.h "$old" "$new" || exit 127
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Debug >/dev/null || exit 127
cmake --build "$build" -j4 --target completion_map_test || exit 127
echo "mutant built: $name"
"$build/completion_map_test" "$build/mutant-$name-state"
