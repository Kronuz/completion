#!/usr/bin/env python3
# Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
# SPDX-License-Identifier: MIT
"""Mutation check of the owner lock.

Applies each of 14 small breaks to the owner-lock code (include/completion/posix.h and io.h)
in a scratch copy of the repository, rebuilds the CTest suite, runs it, and requires the
outcome to be the one expected: the suite fails (the mutant is killed) or, for the mutants
whose break nothing can observe, passes (it survives). The 14 are the ones the CL-2 review
(docs/reviews/cl-2-code.md, finding CL2-5) listed; each is an exact-anchor replacement that
must match exactly once.

  python3 tools/mutants.py                run all, in a copy under the temp directory
  python3 tools/mutants.py --only m1,m6   run some
  python3 tools/mutants.py --list         print them
  python3 tools/mutants.py --check        verify every anchor occurs exactly once (no build; CTest
                                          test completion_mutants_anchors)

A run configures the copy once (it fetches metrics and allocators like any build), checks the
unmutated suite passes, and then spends one build and one ctest per mutant. The working tree
is never touched. Exit status 0 means every mutant ended as expected.
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POSIX = "include/completion/posix.h"
IO = "include/completion/io.h"


class Mutant:
    def __init__(self, ident, title, path, old, new, expect="killed", why=""):
        self.ident, self.title, self.path, self.old, self.new = ident, title, path, old, new
        self._expect, self.why = expect, why

    def expected(self):
        """'killed' or 'survived' on this platform; a dict maps a platform.system() name to one."""
        if isinstance(self._expect, dict):
            return self._expect.get(platform.system(), self._expect["default"])
        return self._expect


def lines(*parts):
    return "\n".join(parts)


# The anchors are the owner-lock code of PosixIO::lock_owner, lock_exclusive and Owner.
DIRECTORY_STEP = '\t\tif (directory) { lock_exclusive(directory_, "lock journal directory", "directories"); }'
FILE_STEP = lines(
    '\t\tif (file) {',
    '\t\t\tconst bool create = options.owner_file == OwnerFile::create_exclusive;',
    '\t\t\towner->hold_file(open_file("owner.lock", create));',
    '\t\t\tlock_exclusive(owner->fd(), "lock journal owner", "files");',
    '\t\t\towner_fd_.store(owner->fd());',
    '\t\t\tif (create) { detail::file_sync(owner->fd()); }',
    '\t\t}')
FILE_CLAIM = '\t\tif (file && !owner->claim_file()) { throw std::logic_error("journal directory already owned"); }'
DIRECTORY_CLAIM = '\t\tif (directory && !owner->claim_directory()) { throw std::logic_error("journal directory already locked"); }'
FLOCK_RETRY = '\t\tdo { result = flock_(fd, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);'
UNSUPPORTED = '\t\tif (error == EOPNOTSUPP || error == ENOTSUP) {'
RELEASE_FILE = lines(
    '\t\t\tif (fd_ >= 0) {',
    '\t\t\t\tint expected = fd_;',
    '\t\t\t\tio_.owner_fd_.compare_exchange_strong(expected, -1);',
    '\t\t\t\t::close(fd_);',
    '\t\t\t}',
    '\t\t\tif (file_) { io_.file_claimed_.store(false); }')
RELEASE_DIRECTORY = '\t\t\tif (directory_) { (void)::flock(io_.directory_, LOCK_UN); io_.directory_claimed_.store(false); }'

MUTANTS = [
    Mutant("m1", "file before directory", POSIX,
           DIRECTORY_STEP + "\n" + FILE_STEP, FILE_STEP + "\n" + DIRECTORY_STEP,
           why="a competing process wrote owner.lock while the directory was held"),
    Mutant("m2", "no EINTR retry", POSIX, FLOCK_RETRY, '\t\tresult = flock_(fd, LOCK_EX | LOCK_NB);',
           why="an interrupted flock escapes"),
    Mutant("m3", "file-only fallback on an unsupported directory", POSIX, DIRECTORY_STEP,
           lines('\t\tif (directory) {',
                 '\t\t\ttry { lock_exclusive(directory_, "lock journal directory", "directories"); }',
                 '\t\t\tcatch (const std::system_error& error) { if (error.code() != std::errc::not_supported) { throw; } }',
                 '\t\t}'),
           why="both on a filesystem that does not lock directories must refuse"),
    Mutant("m4", "no held checks", POSIX,
           FILE_CLAIM + "\n" + DIRECTORY_CLAIM,
           '\t\tif (file) { owner->claim_file(); }\n\t\tif (directory) { owner->claim_directory(); }',
           why="a second acquisition through one IO is busy, not refused"),
    Mutant("m5", "directory lock not released on unwind", POSIX, RELEASE_DIRECTORY,
           '\t\t\tif (directory_) { io_.directory_claimed_.store(false); }',
           why="a failed both leaves the directory locked"),
    Mutant("m6", "EOPNOTSUPP not mapped to not_supported", POSIX, UNSUPPORTED, '\t\tif (error == ENOTSUP) {',
           expect={"Darwin": "killed", "default": "survived"},
           why="macOS has distinct ENOTSUP (45) and EOPNOTSUPP (102); Linux and FreeBSD define them equal"),
    Mutant("m7", "both treated as file-only", POSIX,
           '\t\tconst bool directory = options.mode != OwnerLockMode::file;',
           '\t\tconst bool directory = options.mode == OwnerLockMode::directory;',
           why="a directory holder and a both contender acquire together"),
    Mutant("m8", "directory released before the file", POSIX,
           RELEASE_FILE + "\n" + RELEASE_DIRECTORY, RELEASE_DIRECTORY + "\n" + RELEASE_FILE,
           expect="survived",
           why="release order is liveness only (a both contender sees a spurious busy); nothing observes it"),
    Mutant("m9", "no file sync after create", POSIX,
           '\t\t\tif (create) { detail::file_sync(owner->fd()); }', '',
           expect="survived",
           why="durability of the created owner.lock is unobservable without a sync seam"),
    Mutant("m10", "owner descriptor leaked on release", POSIX,
           lines('\t\t\t\tio_.owner_fd_.compare_exchange_strong(expected, -1);', '\t\t\t\t::close(fd_);'),
           '\t\t\t\tio_.owner_fd_.compare_exchange_strong(expected, -1);',
           why="the descriptor count grows by one per acquisition"),
    Mutant("m11", "legacy file preset becomes both", IO,
           '\t\treturn {OwnerLockMode::file, create ? OwnerFile::create_exclusive : OwnerFile::open_existing};',
           '\t\treturn {OwnerLockMode::both, create ? OwnerFile::create_exclusive : OwnerFile::open_existing};',
           why="the unsafe legacy file/directory control stops acquiring"),
    Mutant("m12", "no directory held check", POSIX, DIRECTORY_CLAIM,
           '\t\tif (directory) { owner->claim_directory(); }',
           why="a second directory lock through one IO is busy, not refused"),
    Mutant("m13", "busy reported as not supported", POSIX, UNSUPPORTED,
           '\t\tif (error == EWOULDBLOCK || error == EOPNOTSUPP || error == ENOTSUP) {',
           why="a held lock reads as an unsupported filesystem"),
    Mutant("m14", "owner_fd_ not recorded", POSIX, '\t\t\towner_fd_.store(owner->fd());', '',
           expect={"Darwin": "killed", "default": "survived"},
           why="only the macOS directory sync reads owner_fd_ (it flushes through the owner file)"),
]


def check_anchors():
    status = 0
    for m in MUTANTS:
        text = (ROOT / m.path).read_text()
        count = text.count(m.old)
        if count != 1:
            print(f"{m.ident}: anchor occurs {count} times in {m.path}", file=sys.stderr)
            status = 1
        if m.old == m.new:
            print(f"{m.ident}: replacement equals the anchor", file=sys.stderr)
            status = 1
    if status == 0:
        print(f"all {len(MUTANTS)} anchors occur exactly once")
    return status


def run(command, cwd, timeout=None):
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)


def failed_tests(output):
    return re.findall(r"^\s*\d+ - (\S+) \((?:Failed|Timeout|SEGFAULT|Subprocess aborted|Exception)", output, re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify the anchors only; no build")
    parser.add_argument("--list", action="store_true", help="print the mutants")
    parser.add_argument("--only", help="comma-separated mutant ids, such as m1,m6")
    parser.add_argument("--jobs", type=int, default=4, help="parallel build jobs (default 4)")
    parser.add_argument("--keep", action="store_true", help="keep the scratch copy and print its path")
    parser.add_argument("--cmake-arg", action="append", default=[], help="extra configure argument, repeatable (a compiler: -DCMAKE_CXX_COMPILER=clang++)")
    args = parser.parse_args()

    if args.check:
        return check_anchors()
    selected = MUTANTS
    if args.only:
        wanted = set(args.only.split(","))
        selected = [m for m in MUTANTS if m.ident in wanted]
        if len(selected) != len(wanted):
            print("unknown mutant id in --only", file=sys.stderr)
            return 2
    if args.list:
        for m in selected:
            print(f"{m.ident:4} {m.expected():9} {m.title}: {m.why}")
        return 0
    if check_anchors():
        return 2

    work = Path(tempfile.mkdtemp(prefix="completion-mutants-"))
    sources = work / "src"
    try:
        shutil.copytree(ROOT, sources, ignore=shutil.ignore_patterns(".git", "build*", "docs"))
        build = work / "build"
        configure = ["cmake", "-S", str(sources), "-B", str(build), "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                     "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g0"] + args.cmake_arg
        result = run(configure, work)
        if result.returncode:
            print(result.stdout)
            return 2

        def fresh_state():
            # A killed run can stop before it cleans its state directory, and the next run
            # would then fail on the leftovers rather than on its own mutant.
            for state in build.glob("*-state"):
                shutil.rmtree(state, ignore_errors=True)

        def build_and_test():
            built = run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], work)
            if built.returncode:
                return "build error", [], built.stdout
            fresh_state()
            tested = run(["ctest", "--test-dir", str(build), "--output-on-failure", "--timeout", "180",
                          "-E", "completion_mutants_anchors"], work)
            if tested.returncode == 0:
                return "survived", [], tested.stdout
            # Killed only if the failure repeats on clean state.
            failed = failed_tests(tested.stdout)
            fresh_state()
            again = run(["ctest", "--test-dir", str(build), "--output-on-failure", "--timeout", "180",
                         "-R", "^(" + "|".join(failed) + ")$"], work) if failed else tested
            return ("killed" if again.returncode else "not reproduced"), failed, tested.stdout

        outcome, failed, output = build_and_test()
        if outcome != "survived":
            print(f"the unmutated suite does not pass ({outcome}: {' '.join(failed)}):\n{output[-4000:]}")
            return 2
        print("unmutated suite passes")

        originals = {}
        bad = 0
        for m in selected:
            path = sources / m.path
            originals.setdefault(m.path, path.read_text())
            path.write_text(originals[m.path].replace(m.old, m.new, 1))
            try:
                outcome, failed, output = build_and_test()
            finally:
                path.write_text(originals[m.path])
            expected = m.expected()
            verdict = "ok" if outcome == expected else "UNEXPECTED"
            bad += verdict != "ok"
            detail = ("by " + ", ".join(failed)) if failed else ""
            print(f"{m.ident:4} {outcome:11} (expected {expected:8}) {verdict:10} {m.title} {detail}", flush=True)
            if outcome == "build error":
                print(output[-1500:])
        killed = sum(1 for m in selected if m.expected() == "killed")
        print(f"{len(selected)} mutants, {killed} expected killed here, {bad} unexpected")
        return 1 if bad else 0
    finally:
        if args.keep:
            print(f"scratch copy: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
