#!/usr/bin/env python3
"""Exercise the vector figure with real LVGL 8.4 and a 48 KiB heap.

Install the pinned component with idf.py reconfigure or set LVGL_SOURCE_DIR.
STICK_FIGURE_PREVIEW_DIR optionally saves native-resolution PPM frames and,
when ffmpeg is available, a looping stick_figure.gif and mood_comparison.png.
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
    preview = os.environ.get("STICK_FIGURE_PREVIEW_DIR")
    if preview:
        Path(preview).mkdir(parents=True, exist_ok=True)
    cache = ROOT / ".cache" / "test_stick_figure-O3-sanitizers"
    cache.mkdir(parents=True, exist_ok=True)
    with nullcontext(str(cache)) as directory:
        temporary = Path(directory)
        write_if_changed(temporary / "lv_conf.h",
            "#ifndef LV_CONF_H\n#define LV_CONF_H\n"
            "#define LV_COLOR_DEPTH 16\n#define LV_MEM_SIZE (48U * 1024U)\n"
            "#define LV_BUILD_EXAMPLES 0\n#define LV_USE_LOG 0\n#endif\n")
        write_if_changed(temporary / "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.16)\nproject(stick_figure_test C CXX)\n"
            'set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fsanitize=address,undefined")\n'
            'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fsanitize=address,undefined")\n'
            'set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)\n'
            f'add_subdirectory("{lvgl}" lvgl EXCLUDE_FROM_ALL)\n'
            "foreach(duration IN ITEMS 5000 600 0)\n"
            f'  add_executable(stick_figure_${{duration}} "{ROOT / "tests" / "test_stick_figure.cpp"}" '
            f'"{ROOT / "tests" / "bodoni_48_smallcaps.c"}")\n'
            "  target_compile_features(stick_figure_${duration} PRIVATE cxx_std_17)\n"
            f'  target_include_directories(stick_figure_${{duration}} PRIVATE "{ROOT / "main"}")\n'
            "  target_compile_definitions(stick_figure_${duration} PRIVATE ODOMETER_ANIMATION_DURATION_MS=${duration})\n"
            "  target_link_libraries(stick_figure_${duration} PRIVATE lvgl)\nendforeach()\n")
        subprocess.run(["cmake", "-Wno-author", "-S", str(temporary), "-B", str(temporary / "build")],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", str(temporary / "build"), "--target",
                        "stick_figure_5000", "stick_figure_600", "stick_figure_0", "--parallel", "4"],
                       check=True, stdout=subprocess.DEVNULL)
        for duration in (5000, 600, 0):
            command = [str(temporary / "build" / f"stick_figure_{duration}")]
            if duration == 5000 and preview:
                command.append(preview)
            environment = dict(os.environ)
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            subprocess.run(command, check=True, env=environment)
    if preview and shutil.which("ffmpeg"):
        output = Path(preview) / "stick_figure.gif"
        subprocess.run(
            ["ffmpeg", "-loglevel", "error", "-y", "-framerate", "12.5", "-i",
             str(Path(preview) / "frame_%04d.ppm"), "-filter_complex",
             "[0:v]split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=none",
             "-loop", "0", str(output)], check=True)
        print(f"Preview: {output}", flush=True)
        mood_inputs = []
        for name in ("increase_happy", "decrease_sad", "negative_increase_happy", "negative_decrease_sad"):
            mood_inputs.extend(["-i", str(Path(preview) / f"{name}.ppm")])
        comparison = Path(preview) / "mood_comparison.png"
        subprocess.run(
            ["ffmpeg", "-loglevel", "error", "-y", *mood_inputs, "-filter_complex",
             "xstack=inputs=4:layout=0_0|w0_0|0_h0|w0_h0", "-frames:v", "1", str(comparison)], check=True)
        print(f"Mood comparison: {comparison}", flush=True)


if __name__ == "__main__":
    main()
