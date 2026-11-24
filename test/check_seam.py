#!/usr/bin/env python3
# Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
# SPDX-License-Identifier: MIT
"""Greps the seam's shape (CTest test completion_seam_shape).

  1. One synchronous loop: `drive_synchronously` is the only loop that runs an operation's primitives on the
     calling thread. execute_primitive() is called from it, from the two queues' fallback workers, and from
     HeldCompletion::execute, which runs one primitive its owner chose and loops over nothing.
  2. InlineCompletion::run has no loop of its own: it calls drive_synchronously.
  3. Every file sync goes through the IO seam: `->sync()` and `->sync_data()` on a File occur only in io.h (the
     defaults of sync_files and sync_data_files) and posix.h (PosixIO's fallback for a foreign file).
  4. With --base DIR (a checkout of the commit before the seam): every file under test/ that exists there is
     byte-identical here except the ones listed in CHANGED.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "include" / "completion"
# Existing suites edited for the seam, each with the reason.
CHANGED = {
    "test/asio_extension_test.cc": "its all-primitives operation gains a SyncData step (24 steps, one per extension slot)",
}


def fail(message: str) -> None:
    print(f"seam shape: {message}", file=sys.stderr)
    sys.exit(1)


def calls(text: str, name: str) -> list[int]:
    return [m.start() for m in re.finditer(rf"(?<![A-Za-z0-9_]){name}\(", text)]


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    depth, i = 0, text.index("{", start)
    begin = i
    while True:
        depth += text[i] == "{"
        depth -= text[i] == "}"
        i += 1
        if depth == 0:
            return text[begin:i]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", help="a checkout of the commit before the seam")
    args = parser.parse_args()

    places = {}
    for path in sorted(INCLUDE.glob("*.h")):
        text = path.read_text()
        for offset in calls(text, "execute_primitive"):
            line = text.count("\n", 0, offset) + 1
            places.setdefault(path.name, []).append(line)
    expected = {"operation.h": 2, "linux_completion.h": 1, "bsd_completion.h": 1, "seam.h": 1}
    found = {name: len(lines) for name, lines in places.items()}
    if found != expected:
        fail(f"execute_primitive() call sites {found}, expected {expected}")

    operation = (INCLUDE / "operation.h").read_text()
    loop = function_body(operation, "bool drive_synchronously(IOOperation &operation, Proceed &&proceed)")
    if "execute_primitive(" not in loop or loop.count("while") != 1:
        fail("drive_synchronously is not the one loop")
    if len(re.findall(r"drive_synchronously\(IOOperation &operation\)", operation)) != 1:
        fail("the one-argument drive_synchronously is declared more than once")

    seam = (INCLUDE / "seam.h").read_text()
    run = function_body(seam, "class InlineCompletion")
    if "drive_synchronously(" not in run or re.search(r"\b(while|for)\b", run):
        fail("InlineCompletion::run has a loop of its own or does not call drive_synchronously")
    held = function_body(seam, "class HeldCompletion")
    if "drive_synchronously(" in held:
        fail("HeldCompletion drives synchronously")

    direct = {}
    for path in sorted(INCLUDE.glob("*.h")):
        count = len(re.findall(r"(?:->|\.)sync(?:_data)?\(\)", path.read_text()))
        if count:
            direct[path.name] = count
    # io.h: the default sync_files and sync_data_files; posix.h: batch_fds, the one place a PosixIO
    # batch lets a file sync itself (another backend's file, or one the batch would weaken).
    if direct != {"io.h": 2, "posix.h": 1}:
        fail(f"File::sync or File::sync_data called outside the seam: {direct}, expected io.h 2 and posix.h 1")

    if args.base:
        base = Path(args.base)
        for path in sorted((base / "test").iterdir()):
            relative = f"test/{path.name}"
            here = ROOT / relative
            if not here.exists():
                fail(f"{relative} was removed")
            if here.read_bytes() != path.read_bytes() and relative not in CHANGED:
                fail(f"{relative} changed and is not listed in CHANGED")
        for relative, why in CHANGED.items():
            if (ROOT / relative).read_bytes() == (base / relative).read_bytes():
                fail(f"{relative} is listed as changed ({why}) but is not")
    print("seam shape: ok")


if __name__ == "__main__":
    main()
