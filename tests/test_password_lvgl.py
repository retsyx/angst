#!/usr/bin/env python3
"""Render dummy passwords with the actual pinned LVGL, rather than GUI mocks."""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import subprocess


TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent
LVGL = ROOT / "managed_components/lvgl__lvgl"
FLAGS = [
    "-DLV_CONF_SKIP", "-DLV_KCONFIG_IGNORE", "-DLV_MEM_CUSTOM=1", "-DLV_MEMCPY_MEMSET_STD=1",
    "-DLV_FONT_MONTSERRAT_40=1", "-DLV_COLOR_DEPTH=16", "-DLV_COLOR_16_SWAP=1",
    "-DLV_USE_LOG=0", "-O3", "-g", "-fsanitize=address,undefined",
]


def main():
    if not (LVGL / "src/widgets/lv_label.c").is_file():
        raise SystemExit("Build the firmware once to install its pinned LVGL dependency first.")
    fingerprint = hashlib.sha256(" ".join(FLAGS).encode()).hexdigest()[:12]
    cache = ROOT / ".cache" / ("password-lvgl-" + fingerprint)
    cache.mkdir(parents=True, exist_ok=True)
    headers = cache / "headers"
    (headers / "freertos").mkdir(parents=True, exist_ok=True)
    (headers / "esp_err.h").write_text(
        "#pragma once\n"
        "typedef int esp_err_t;\n"
        "#define ESP_OK 0\n"
        "#define ESP_ERR_NO_MEM 0x101\n"
        "#define ESP_ERR_INVALID_ARG 0x102\n"
        "#define ESP_ERR_INVALID_STATE 0x103\n"
    )
    (headers / "freertos/FreeRTOS.h").write_text(
        "#pragma once\n#include <cassert>\n"
        "typedef unsigned portMUX_TYPE;\n"
        "#define portMUX_INITIALIZER_UNLOCKED 0\n"
        "#define portENTER_CRITICAL(lock) (++*(lock))\n"
        "#define portEXIT_CRITICAL(lock) do { assert(*(lock)); --*(lock); } while (0)\n"
    )
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    sources = sorted((LVGL / "src").rglob("*.c"))
    # The cache is only a compilation optimization. Any LVGL header or source
    # change invalidates the objects; the app and harness are always recompiled.
    dependency_time = max(path.stat().st_mtime_ns for path in LVGL.rglob("*.[ch]"))

    def compile_source(source):
        name = str(source.relative_to(LVGL)).replace("/", "_") + ".o"
        output = cache / name
        if not output.exists() or output.stat().st_mtime_ns < dependency_time:
            subprocess.run(
                [cc, "-std=c11", *FLAGS, "-I" + str(LVGL), "-c", str(source), "-o", str(output)],
                check=True,
            )
        return output

    with ThreadPoolExecutor(max_workers=4) as executor:
        objects = list(executor.map(compile_source, sources))
    executable = cache / "test_password_lvgl"
    subprocess.run(
        [cxx, "-std=c++17", *FLAGS, "-Wall", "-Wextra", "-Werror",
         "-I" + str(headers), "-I" + str(LVGL),
         str(TESTS / "test_password_lvgl.cpp"), *map(str, objects), "-o", str(executable)],
        check=True,
    )
    environment = os.environ.copy()
    environment["UBSAN_OPTIONS"] = "halt_on_error=1"
    subprocess.run([str(executable)], check=True, env=environment)


if __name__ == "__main__":
    main()
