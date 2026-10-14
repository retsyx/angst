#!/usr/bin/env python3
"""Exercise independent digit and figure switches with real LVGL under sanitizers."""

from pathlib import Path
import os
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
    cache = ROOT / ".cache" / "test_animation_switches-O3-sanitizers"
    cache.mkdir(parents=True, exist_ok=True)
    with nullcontext(str(cache)) as directory:
        temporary = Path(directory)
        write_if_changed(temporary / "lv_conf.h",
            "#ifndef LV_CONF_H\n#define LV_CONF_H\n"
            "#define LV_COLOR_DEPTH 16\n#define LV_MEM_SIZE (48U * 1024U)\n"
            "#define LV_BUILD_EXAMPLES 0\n#define LV_USE_LOG 0\n#endif\n")
        write_if_changed(temporary / "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.16)\nproject(animation_switches_test C CXX)\n"
            'set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fsanitize=address,undefined")\n'
            'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fsanitize=address,undefined")\n'
            'set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)\n'
            f'add_subdirectory("{lvgl}" lvgl EXCLUDE_FROM_ALL)\n'
            "foreach(duration IN ITEMS 5000 0)\n"
            f'  add_executable(switches_${{duration}} "{ROOT / "tests" / "test_animation_switches.cpp"}" '
            f'"{ROOT / "tests" / "bodoni_48_smallcaps.c"}")\n'
            "  target_compile_features(switches_${duration} PRIVATE cxx_std_17)\n"
            f'  target_include_directories(switches_${{duration}} PRIVATE "{ROOT / "main"}")\n'
            "  target_compile_definitions(switches_${duration} PRIVATE ODOMETER_ANIMATION_DURATION_MS=${duration})\n"
            "  target_link_libraries(switches_${duration} PRIVATE lvgl)\nendforeach()\n")
        subprocess.run(["cmake", "-Wno-author", "-S", str(temporary), "-B", str(temporary / "build")],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", str(temporary / "build"), "--target",
                        "switches_5000", "switches_0", "--parallel", "4"],
                       check=True, stdout=subprocess.DEVNULL)
        environment = dict(os.environ)
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
        for duration in (5000, 0):
            subprocess.run([str(temporary / "build" / f"switches_{duration}")],
                           check=True, env=environment)


if __name__ == "__main__":
    main()
