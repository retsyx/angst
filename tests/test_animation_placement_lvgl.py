#!/usr/bin/env python3
"""Check native full-screen movement and glyph transparency with real LVGL.

The suite compares cached price rendering with ordinary LVGL labels, including
antialiasing, punctuation gaps, digit counters, and animated reels.
"""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LVGL = ROOT / "managed_components/lvgl__lvgl"
FLAGS = [
    "-DLV_CONF_SKIP", "-DLV_KCONFIG_IGNORE", "-DLV_MEM_CUSTOM=1", "-DLV_MEMCPY_MEMSET_STD=1",
    "-DLV_COLOR_DEPTH=16", "-DLV_COLOR_16_SWAP=1", "-DLV_USE_LOG=0",
    "-DLV_DISP_DEF_REFR_PERIOD=16", "-O3", "-g", "-fsanitize=address,undefined",
]


def main():
    if not (LVGL / "src/widgets/lv_label.c").is_file():
        raise SystemExit("Build the firmware once to install its pinned LVGL dependency first.")
    fingerprint = hashlib.sha256(" ".join(FLAGS).encode()).hexdigest()[:12]
    cache = ROOT / ".cache" / ("animation-placement-lvgl-" + fingerprint)
    cache.mkdir(parents=True, exist_ok=True)
    shared_cache = ROOT / ".cache" / ("odometer-lvgl-" + fingerprint)
    dependency_time = max(path.stat().st_mtime_ns for path in LVGL.rglob("*.[ch]"))
    cc, cxx = os.environ.get("CC", "cc"), os.environ.get("CXX", "c++")

    def compile_source(source):
        name = str(source.relative_to(LVGL)).replace("/", "_") + ".o"
        shared = shared_cache / name
        # Reuse matching LVGL objects without writing another suite's cache.
        if shared.is_file() and shared.stat().st_mtime_ns >= dependency_time:
            return shared
        output = cache / name
        if not output.is_file() or output.stat().st_mtime_ns < dependency_time:
            subprocess.run([cc, "-std=c11", *FLAGS, "-I" + str(LVGL), "-c", str(source),
                            "-o", str(output)], check=True)
        return output

    with ThreadPoolExecutor(max_workers=4) as executor:
        objects = list(executor.map(compile_source, sorted((LVGL / "src").rglob("*.c"))))
    font = cache / "bodoni_72_smallcaps.o"
    subprocess.run([cc, "-std=c11", *FLAGS, "-I" + str(LVGL), "-c",
                    str(ROOT / "main/bodoni_72_smallcaps.c"), "-o", str(font)], check=True)
    executable = cache / "test_animation_placement_lvgl"
    subprocess.run([cxx, "-std=c++17", *FLAGS, "-Wall", "-Wextra", "-Werror",
                    "-I" + str(LVGL), "-I" + str(ROOT / "main"),
                    str(ROOT / "tests/test_animation_placement_lvgl.cpp"),
                    *map(str, objects), str(font), "-o", str(executable)], check=True)
    environment = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    subprocess.run([str(executable)], check=True, env=environment)


if __name__ == "__main__":
    main()
