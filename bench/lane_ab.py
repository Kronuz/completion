#!/usr/bin/env python3
# Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
# SPDX-License-Identifier: MIT
"""Interleaved A/B runs of completion_lane_probe from two builds.

  python3 bench/lane_ab.py --a BIN --b BIN [--rounds 9] [--dir DIR] [--env-b NAME=VALUE] -- BULK TURNS CHUNK_KIB

Each round runs A then B, or B then A on alternate rounds, each on a fresh scratch directory under DIR. The
probe prints one line per run; per arm this prints the median across rounds of every column, the
[min, max] across rounds, and the raw lines. With --a and --b the same binary it is the A/A band.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys

COLUMNS = [
    ("wait p50", r"admission wait ms p50 ([\d.]+)"),
    ("wait p99", r"admission wait ms p50 [\d.]+ p99 ([\d.]+)"),
    ("total p50", r"total ms p50 ([\d.]+)"),
    ("total p99", r"total ms p50 [\d.]+ p99 ([\d.]+)"),
    ("bulk MiB/s", r"([\d.]+) MiB/s"),
]


def run(binary: str, directory: str, extra_env: dict, probe_args: list) -> str:
    env = dict(os.environ)
    env.update(extra_env)
    result = subprocess.run([binary, directory, *probe_args], capture_output=True, text=True, env=env, check=True)
    return result.stdout.strip()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", required=True)
    parser.add_argument("--b", required=True)
    parser.add_argument("--rounds", type=int, default=9)
    parser.add_argument("--dir", default=os.environ.get("TMPDIR", "."))
    parser.add_argument("--env-a", action="append", default=[])
    parser.add_argument("--env-b", action="append", default=[])
    parser.add_argument("probe_args", nargs="*")
    args = parser.parse_args()
    env = {"a": dict(item.split("=", 1) for item in args.env_a), "b": dict(item.split("=", 1) for item in args.env_b)}
    binaries = {"a": args.a, "b": args.b}
    lines = {"a": [], "b": []}
    for round_number in range(args.rounds):
        order = ("a", "b") if round_number % 2 == 0 else ("b", "a")
        for arm in order:
            line = run(binaries[arm], args.dir, env[arm], args.probe_args)
            lines[arm].append(line)
            print(f"round {round_number} {arm}: {line}", flush=True)
    for arm in ("a", "b"):
        print(f"arm {arm} ({binaries[arm]}, env {env[arm]}), {args.rounds} rounds, probe {' '.join(args.probe_args)}")
        for name, pattern in COLUMNS:
            values = [float(re.search(pattern, line).group(1)) for line in lines[arm]]
            print(f"  {name:11s} median {statistics.median(values):9.3f}   [{min(values):9.3f}, {max(values):9.3f}]")


if __name__ == "__main__":
    sys.exit(main())
