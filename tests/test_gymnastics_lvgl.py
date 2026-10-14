#!/usr/bin/env python3
"""Check native gymnastics movement, actual LVGL poses and rendered previews."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent
LVGL = ROOT / "managed_components/lvgl__lvgl"
FLAGS = [
    "-DLV_CONF_SKIP", "-DLV_KCONFIG_IGNORE", "-DLV_MEM_CUSTOM=1", "-DLV_MEMCPY_MEMSET_STD=1",
    "-DLV_COLOR_DEPTH=16", "-DLV_COLOR_16_SWAP=1", "-DLV_USE_LOG=0",
    "-DLV_DISP_DEF_REFR_PERIOD=16", "-O3", "-g", "-fsanitize=address,undefined",
]


def preview_artifacts(preview):
    if shutil.which("ffmpeg"):
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-framerate", "7.8125",
                        "-i", str(preview / "native_%04d.ppm"), "-filter_complex",
                        "[0:v]split[a][b];[a]palettegen[p];[b][p]paletteuse=dither=none",
                        "-loop", "0", str(preview / "gymnastics.gif")], check=True)
        poses = [preview / ("pose_" + name + ".ppm") for name in
                 ("cartwheel", "jump", "somersault", "handspring")]
        inputs = [argument for pose in poses for argument in ("-i", str(pose))]
        filters = ";".join(f"[{index}:v]crop=100:90:218:140,scale=300:270:flags=neighbor[p{index}]"
                           for index in range(4)) + ";[p0][p1][p2][p3]hstack=inputs=4"
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *inputs, "-filter_complex", filters,
                        "-frames:v", "1", str(preview / "gymnastics-poses.png")], check=True)
        print(f"Gymnastics previews: {preview}")
        frames = sorted(preview.glob("native_*.ppm"))
        selected = [frames[round((len(frames) - 1) * index / 7)] for index in range(8)]
        inputs = [argument for frame in selected for argument in ("-i", str(frame))]
        layout = "|".join(f"{(index % 2) * 536}_{(index // 2) * 240}" for index in range(8))
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *inputs, "-filter_complex",
                        "xstack=inputs=8:layout=" + layout, "-frames:v", "1",
                        str(preview / "gymnastics-native-contact-sheet.png")], check=True)


def main():
    if not (LVGL / "src/widgets/lv_label.c").is_file():
        raise SystemExit("Build the firmware once to install its pinned LVGL dependency first.")
    fingerprint = hashlib.sha256(" ".join(FLAGS).encode()).hexdigest()[:12]
    cache = ROOT / ".cache" / ("odometer-lvgl-" + fingerprint)
    cache.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    sources = sorted((LVGL / "src").rglob("*.c"))
    dependency_time = max(path.stat().st_mtime_ns for path in LVGL.rglob("*.[ch]"))

    def compile_source(source):
        output = cache / (str(source.relative_to(LVGL)).replace("/", "_") + ".o")
        if not output.exists() or output.stat().st_mtime_ns < dependency_time:
            subprocess.run(
                [cc, "-std=c11", *FLAGS, "-I" + str(LVGL), "-c", str(source), "-o", str(output)],
                check=True,
            )
        return output

    with ThreadPoolExecutor(max_workers=4) as executor:
        objects = list(executor.map(compile_source, sources))
    font = cache / "bodoni_72_smallcaps.o"
    subprocess.run(
        [cc, "-std=c11", *FLAGS, "-I" + str(LVGL), "-c",
         str(ROOT / "main/bodoni_72_smallcaps.c"), "-o", str(font)],
        check=True,
    )
    executable = cache / "test_gymnastics_lvgl"
    subprocess.run(
        [cxx, "-std=c++17", *FLAGS, "-Wall", "-Wextra", "-Werror",
         "-I" + str(LVGL), "-I" + str(ROOT / "main"), str(TESTS / "test_gymnastics_lvgl.cpp"),
         *map(str, objects), str(font), "-o", str(executable)],
        check=True,
    )
    environment = os.environ.copy()
    environment["UBSAN_OPTIONS"] = "halt_on_error=1"
    preview = ROOT / ".cache/gymnastics-preview"
    preview.mkdir(parents=True, exist_ok=True)
    for frame in preview.glob("native_*.ppm"):
        frame.unlink()
    subprocess.run([str(executable), str(preview)], check=True, env=environment)
    preview_artifacts(preview)


if __name__ == "__main__":
    main()
