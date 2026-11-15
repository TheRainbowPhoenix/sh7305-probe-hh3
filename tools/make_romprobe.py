#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Artem Novak
# SPDX-License-Identifier: BSD-3-Clause
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: make_romprobe.py OUTPUT")
path = Path(sys.argv[1])
path.parent.mkdir(parents=True, exist_ok=True)
with path.open("wb") as f:
    f.truncate(0x180000)
    for offset, value in ((0, 0x5a), (0x080000, 0xa5), (0x17ffff, 0x3c)):
        f.seek(offset)
        f.write(bytes([value]))
