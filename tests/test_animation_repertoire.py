#!/usr/bin/env python3
"""Render all 33 scenes and check scheduling with real LVGL and a 48 KiB heap.

ANIMATION_PREVIEW_DIR saves native 240x135 PPM frames. With ffmpeg available,
it also produces a numbered contact sheet and a short GIF for each animation.
The same suite covers normal and zero-duration odometer updates, scenes at
0.5x/1x/2x, and a padded root under ASan and UBSan. Price protection covers
real rendered pixels, clear scene placement, roaming, the four intentional
number interactions, and flying behind the price's opaque glyphs while
remaining visible through their gaps. Use LVGL_SOURCE_DIR to override the managed
LVGL 8.4 component.
"""

from pathlib import Path
import os
import shutil
import subprocess
from contextlib import nullcontext


ROOT = Path(__file__).resolve().parents[1]


def preview_artifacts(directory):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return
    for animation in range(1, 34):
        subprocess.run(
            [ffmpeg, "-loglevel", "error", "-y", "-framerate", "10", "-i",
             str(directory / f"animation_{animation:02d}_%02d.ppm"), "-filter_complex",
             "[0:v]split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=none",
             "-loop", "0", str(directory / f"animation_{animation:02d}.gif")], check=True)

    # A tiny fixed bitmap label avoids fonts and image library dependencies.
    digits = {
        "0": ["111", "101", "101", "101", "111"],
        "1": ["010", "110", "010", "010", "111"],
        "2": ["111", "001", "111", "100", "111"],
        "3": ["111", "001", "111", "001", "111"],
        "4": ["101", "101", "111", "001", "001"],
        "5": ["111", "100", "111", "001", "111"],
        "6": ["111", "100", "111", "101", "111"],
        "7": ["111", "001", "001", "001", "001"],
        "8": ["111", "101", "111", "101", "111"],
        "9": ["111", "101", "111", "001", "111"],
    }
    width, height, columns, rows, label = 240, 135, 4, 9, 15
    full_width = width * columns
    sheet = bytearray(full_width * (height + label) * rows * 3)
    for animation in range(1, 34):
        data = (directory / f"animation_{animation:02d}_14.ppm").read_bytes()
        header = b"P6\n240 135\n255\n"
        assert data.startswith(header)
        rgb = data[len(header):]
        column, row = (animation - 1) % columns, (animation - 1) // columns
        x_start, y_start = column * width, row * (height + label)
        for y in range(height):
            target = ((y_start + label + y) * full_width + x_start) * 3
            sheet[target:target + width * 3] = rgb[y * width * 3:(y + 1) * width * 3]
        for digit_index, digit in enumerate(f"{animation:02d}"):
            for y, cells in enumerate(digits[digit]):
                for x, cell in enumerate(cells):
                    if cell != "1":
                        continue
                    for scale_y in range(2):
                        for scale_x in range(2):
                            px = x_start + 6 + digit_index * 8 + x * 2 + scale_x
                            py = y_start + 2 + y * 2 + scale_y
                            target = (py * full_width + px) * 3
                            sheet[target:target + 3] = b"\xf2\xc8\x7b"
    sheet_ppm = directory / "animation_contact_sheet.ppm"
    sheet_ppm.write_bytes(f"P6\n{full_width} {(height + label) * rows}\n255\n".encode() + sheet)
    sheet_png = directory / "animation_contact_sheet.png"
    subprocess.run([ffmpeg, "-loglevel", "error", "-y", "-i", str(sheet_ppm),
                    "-frames:v", "1", str(sheet_png)], check=True)
    print(f"Animation contact sheet: {sheet_png}", flush=True)
    print(f"Animation previews: {directory}/animation_01.gif through animation_33.gif", flush=True)


def write_if_changed(path, content):
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def main():
    lvgl = Path(os.environ.get("LVGL_SOURCE_DIR", ROOT / "managed_components" / "lvgl__lvgl"))
    if not (lvgl / "lvgl.h").is_file():
        raise SystemExit("Install LVGL with idf.py reconfigure, or set LVGL_SOURCE_DIR to LVGL 8.4.")
    preview = os.environ.get("ANIMATION_PREVIEW_DIR")
    if preview:
        preview = Path(preview).resolve()
        preview.mkdir(parents=True, exist_ok=True)
    cache = ROOT / ".cache" / "test_animation_repertoire-O3-sanitizers"
    cache.mkdir(parents=True, exist_ok=True)
    with nullcontext(str(cache)) as directory:
        temporary = Path(directory)
        write_if_changed(temporary / "lv_conf.h",
            "#ifndef LV_CONF_H\n#define LV_CONF_H\n"
            "#define LV_COLOR_DEPTH 16\n#define LV_MEM_SIZE (48U * 1024U)\n"
            "#define LV_BUILD_EXAMPLES 0\n#define LV_USE_LOG 0\n#endif\n")
        write_if_changed(temporary / "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.16)\nproject(animation_repertoire_test C CXX)\n"
            'set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fsanitize=address,undefined")\n'
            'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fsanitize=address,undefined")\n'
            'set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)\n'
            f'add_subdirectory("{lvgl}" lvgl EXCLUDE_FROM_ALL)\n'
            "foreach(duration IN ITEMS 5000 0)\n"
            f'  add_executable(repertoire_${{duration}} "{ROOT / "tests" / "test_animation_repertoire.cpp"}" '
            f'"{ROOT / "tests" / "bodoni_48_smallcaps.c"}")\n'
            "  target_compile_features(repertoire_${duration} PRIVATE cxx_std_17)\n"
            f'  target_include_directories(repertoire_${{duration}} PRIVATE "{ROOT / "main"}")\n'
            "  target_compile_definitions(repertoire_${duration} PRIVATE ODOMETER_ANIMATION_DURATION_MS=${duration})\n"
            "  target_link_libraries(repertoire_${duration} PRIVATE lvgl)\nendforeach()\n")
        subprocess.run(["cmake", "-Wno-author", "-S", str(temporary), "-B", str(temporary / "build")],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", str(temporary / "build"), "--target",
                        "repertoire_5000", "repertoire_0", "--parallel", "4"],
                       check=True, stdout=subprocess.DEVNULL)
        for duration in (5000, 0):
            command = [str(temporary / "build" / f"repertoire_{duration}")]
            if duration == 5000 and preview:
                command.append(str(preview))
            environment = dict(os.environ)
            environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
            subprocess.run(command, check=True, env=environment)
    if preview:
        preview_artifacts(preview)


if __name__ == "__main__":
    main()
