#!/usr/bin/env python3
"""Render the e-paper UI on the PC, without the board.

Compiles the real components/epaper_ui + components/project_assets code together with
scripts/screen_preview/screen_preview.cpp (sample states for every screen) using a host C++
compiler (zig), runs it, and writes one PNG per screen plus a contact sheet.

Usage:
    python scripts/preview_screens.py                 # every screen
    python scripts/preview_screens.py wifi notas      # only scenes whose name contains a filter
    python scripts/preview_screens.py --scale 2       # PNGs at 2x for easier viewing

Output: build/screen_preview/telas/*.png and build/screen_preview/telas/_todas.png.
Compiler: $ZIG, else D:/esp/tools/ziglang/ziglang/zig.exe, else the `ziglang` pip package
(install with: python -m pip install ziglang --target D:/esp/tools/ziglang).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
COMPONENTS = ROOT / "components"
PREVIEW_SRC = ROOT / "scripts" / "screen_preview" / "screen_preview.cpp"
# Lives inside the existing build/ folder (AGENTS.md: no new build folders); `idf.py
# fullclean` wipes it too, which only costs a rebuild.
BUILD_DIR = ROOT / "build" / "screen_preview"
OBJ_DIR = BUILD_DIR / "obj"
OUT_DIR = BUILD_DIR / "telas"
EXE = BUILD_DIR / ("screen_preview.exe" if os.name == "nt" else "screen_preview")

INCLUDE_DIRS = [
    COMPONENTS / "epaper_ui" / "include",
    COMPONENTS / "epaper_ui",
    COMPONENTS / "design_tokens" / "include",
    COMPONENTS / "project_assets",
    COMPONENTS / "journal_service" / "include",
    COMPONENTS / "journal_view" / "include",
]
CXXFLAGS = ["-std=c++20", "-O1", "-w"]
DEFAULT_ZIG = Path("D:/esp/tools/ziglang/ziglang/zig.exe")


def find_zig() -> list[str]:
    if os.environ.get("ZIG"):
        return [os.environ["ZIG"]]
    if DEFAULT_ZIG.exists():
        return [str(DEFAULT_ZIG)]
    if shutil.which("zig"):
        return ["zig"]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang"]
    except ImportError:
        sys.exit("Compilador zig não encontrado. Instale com:\n"
                 "  python -m pip install ziglang --target D:/esp/tools/ziglang")


def sources() -> list[Path]:
    srcs = sorted((COMPONENTS / "epaper_ui").glob("*.cpp"))
    srcs += sorted((COMPONENTS / "project_assets").glob("*.cpp"))
    # The journal page's view model is pure C++, so the preview shows its real output.
    srcs.append(COMPONENTS / "journal_service" / "journal_period.cpp")
    srcs.append(COMPONENTS / "journal_view" / "journal_view.cpp")
    srcs.append(PREVIEW_SRC)
    return srcs


def newest_header_mtime() -> float:
    newest = 0.0
    for directory in INCLUDE_DIRS:
        for header in directory.rglob("*.h"):
            newest = max(newest, header.stat().st_mtime)
    return newest


def compile_all(zig: list[str]) -> list[Path]:
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    header_mtime = newest_header_mtime()
    includes = [f"-I{d}" for d in INCLUDE_DIRS]

    jobs = []
    objects = []
    for src in sources():
        obj = OBJ_DIR / f"{src.parent.name}__{src.stem}.o"
        objects.append(obj)
        if obj.exists() and obj.stat().st_mtime >= max(src.stat().st_mtime, header_mtime):
            continue
        jobs.append((src, obj))

    def build(job: tuple[Path, Path]) -> tuple[Path, subprocess.CompletedProcess]:
        src, obj = job
        cmd = [*zig, "c++", *CXXFLAGS, *includes, "-c", str(src), "-o", str(obj)]
        return src, subprocess.run(cmd, capture_output=True, text=True)

    if jobs:
        print(f"Compilando {len(jobs)} arquivo(s)...")
    failed = False
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, result in pool.map(build, jobs):
            if result.returncode != 0:
                failed = True
                print(f"\nERRO em {src.relative_to(ROOT)}:\n{result.stderr}", file=sys.stderr)
    if failed:
        sys.exit(1)
    return objects


def link(zig: list[str], objects: list[Path]) -> None:
    if EXE.exists() and all(EXE.stat().st_mtime >= o.stat().st_mtime for o in objects):
        return
    result = subprocess.run([*zig, "c++", *[str(o) for o in objects], "-o", str(EXE)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"Falha ao linkar:\n{result.stderr}")


def contact_sheet(pngs: list[Path], columns: int = 6) -> None:
    thumbs = [Image.open(p).convert("L") for p in pngs]
    tw, th = 240, 400
    label_h, pad = 22, 12
    rows = (len(thumbs) + columns - 1) // columns
    sheet = Image.new("L", (columns * (tw + pad) + pad, rows * (th + label_h + pad) + pad), 200)
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()
    for i, (path, img) in enumerate(zip(pngs, thumbs)):
        x = pad + (i % columns) * (tw + pad)
        y = pad + (i // columns) * (th + label_h + pad)
        sheet.paste(img.resize((tw, th), Image.LANCZOS), (x, y + label_h))
        draw.text((x, y + 4), path.stem, fill=0, font=font)
    sheet.save(OUT_DIR / "_todas.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("filters", nargs="*", help="gera só as telas cujo nome contém o texto")
    parser.add_argument("--scale", type=int, default=1, help="ampliação dos PNGs (padrão 1)")
    args = parser.parse_args()

    zig = find_zig()
    link(zig, compile_all(zig))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if not args.filters:
        for old in OUT_DIR.glob("*.png"):
            old.unlink()

    result = subprocess.run([str(EXE), str(OUT_DIR), *args.filters], capture_output=True,
                            text=True)
    if result.returncode != 0:
        sys.exit(f"Nenhuma tela gerada.\n{result.stderr}")

    pngs = []
    for name in result.stdout.split():
        pbm = OUT_DIR / f"{name}.pbm"
        png = OUT_DIR / f"{name}.png"
        img = Image.open(pbm).convert("L")
        if args.scale > 1:
            img = img.resize((img.width * args.scale, img.height * args.scale), Image.NEAREST)
        img.save(png)
        pbm.unlink()
        pngs.append(png)

    if not args.filters:
        contact_sheet(pngs)
    print(f"{len(pngs)} tela(s) em {OUT_DIR.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
