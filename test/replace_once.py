#!/usr/bin/env python3
# Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
# SPDX-License-Identifier: MIT
"""Replaces one exact text in one file, which must occur exactly once: the mutation step of a mutant job.

  python3 test/replace_once.py FILE OLD NEW
"""
import sys
from pathlib import Path

path, old, new = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    sys.exit(f"{path}: {text.count(old)} occurrences of {old!r}, expected 1")
path.write_text(text.replace(old, new))
