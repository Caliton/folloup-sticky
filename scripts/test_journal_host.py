#!/usr/bin/env python3
"""Build and run the journal host tests (period math) on the PC with zig, no board needed.

Usage: python scripts/test_journal_host.py
Compiler: $ZIG, else D:/esp/tools/ziglang/ziglang/zig.exe, else `zig` on PATH.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JOURNAL = ROOT / "components" / "journal_service"
# Inside the existing build/ folder (AGENTS.md: no new build folders).
OUT = ROOT / "build" / "host_tests" / ("journal_period_test.exe" if os.name == "nt" else "journal_period_test")
DEFAULT_ZIG = Path("D:/esp/tools/ziglang/ziglang/zig.exe")


def zig() -> str:
    if os.environ.get("ZIG"):
        return os.environ["ZIG"]
    if DEFAULT_ZIG.exists():
        return str(DEFAULT_ZIG)
    found = shutil.which("zig")
    if found:
        return found
    sys.exit("zig não encontrado (python -m pip install ziglang --target D:/esp/tools/ziglang)")


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [zig(), "c++", "-std=c++20", "-O1", f"-I{JOURNAL / 'include'}",
           str(JOURNAL / "journal_period.cpp"), str(JOURNAL / "host_test" / "journal_period_test.cpp"),
           "-o", str(OUT)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        sys.exit(f"Falha ao compilar:\n{build.stderr}")
    sys.exit(subprocess.run([str(OUT)]).returncode)


if __name__ == "__main__":
    main()
