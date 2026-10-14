#!/usr/bin/env python3
"""Verify figure scaling with real LVGL 8.4, ASan/UBSan and a 48 KiB heap.

STICK_FIGURE_SCALE_PREVIEW_DIR saves five PPMs and an optional comparison PNG.
STICK_FIGURE_BASELINE_HEADER optionally compares the old unscaled header using
the exact same renderer; persistent golden frames require no old source file.
"""

from pathlib import Path
import os
import shutil
import subprocess
from contextlib import nullcontext


ROOT = Path(__file__).resolve().parents[1]


def write_if_changed(path, content):
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def main():
    lvgl = Path(os.environ.get("LVGL_SOURCE_DIR", ROOT / "managed_components" / "lvgl__lvgl"))
    if not (lvgl / "lvgl.h").is_file():
        raise SystemExit("Install LVGL with idf.py reconfigure, or set LVGL_SOURCE_DIR to LVGL 8.4.")
    preview = os.environ.get("STICK_FIGURE_SCALE_PREVIEW_DIR")
    baseline = os.environ.get("STICK_FIGURE_BASELINE_HEADER")
    if preview:
        Path(preview).mkdir(parents=True, exist_ok=True)
    cache = ROOT / ".cache" / "test_stick_figure_scaling-O3-sanitizers"
    cache.mkdir(parents=True, exist_ok=True)
    with nullcontext(str(cache)) as directory:
        temporary = Path(directory)
        write_if_changed(temporary / "lv_conf.h",
            "#ifndef LV_CONF_H\n#define LV_CONF_H\n"
            "#define LV_COLOR_DEPTH 16\n#define LV_MEM_SIZE (48U * 1024U)\n"
            "#define LV_BUILD_EXAMPLES 0\n#define LV_USE_LOG 0\n#endif\n")
        cmake = (
            "cmake_minimum_required(VERSION 3.16)\nproject(figure_scaling C CXX)\n"
            'set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fsanitize=address,undefined")\n'
            'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fsanitize=address,undefined")\n'
            'set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)\n'
            f'add_subdirectory("{lvgl}" lvgl EXCLUDE_FROM_ALL)\n'
            "function(make_test target)\n"
            f'  add_executable(${{target}} "{ROOT / "tests" / "test_stick_figure_scaling.cpp"}" '
            f'"{ROOT / "tests" / "bodoni_48_smallcaps.c"}")\n'
            "  target_compile_features(${target} PRIVATE cxx_std_17)\n"
            f'  target_include_directories(${{target}} PRIVATE "{ROOT / "main"}")\n'
            "  target_compile_definitions(${target} PRIVATE ODOMETER_ANIMATION_DURATION_MS=600)\n"
            "  target_link_libraries(${target} PRIVATE lvgl)\nendfunction()\nmake_test(scaling)\n")
        targets = ["scaling"]
        if baseline:
            cmake += ('make_test(baseline)\ntarget_compile_definitions(baseline PRIVATE '
                      f'STICK_FIGURE_BASELINE_HEADER="{Path(baseline).resolve()}")\n')
            targets.append("baseline")
        write_if_changed(temporary / "CMakeLists.txt", cmake)
        subprocess.run(["cmake", "-Wno-author", "-S", str(temporary), "-B", str(temporary / "build")],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", str(temporary / "build"), "--target", *targets, "--parallel", "4"],
                       check=True, stdout=subprocess.DEVNULL)
        environment = dict(os.environ)
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        executable = temporary / "build" / "scaling"
        if baseline:
            previous = subprocess.check_output([str(temporary / "build" / "baseline"), "--golden"], text=True, env=environment)
            current = subprocess.check_output([str(executable), "--golden"], text=True, env=environment)
            assert current == previous, "The 1x figure changed geometry, pose, timing or pixels"
            print("PASS 1x matches the original header's rendered frames exactly", flush=True)
            if os.environ.get("STICK_FIGURE_PRINT_GOLDENS"):
                print(previous, end="", flush=True)
        subprocess.run([str(executable), *([preview] if preview else [])], check=True, env=environment)
    if preview and shutil.which("ffmpeg"):
        inputs = []
        for scale in (50, 75, 100, 150, 200):
            inputs.extend(["-i", str(Path(preview) / f"scale_{scale:03}.ppm")])
        output = Path(preview) / "scale_comparison.png"
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *inputs, "-filter_complex",
                        "hstack=inputs=5", "-frames:v", "1", str(output)], check=True)
        print(f"Scale comparison: {output}", flush=True)


if __name__ == "__main__":
    main()
