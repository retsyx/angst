#!/usr/bin/env python3
"""Exercise screen-only password rendering and lifecycle with host API mocks."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent
HEADERS = ("esp_err.h", "lvgl.h", "freertos/FreeRTOS.h")


def main():
    with tempfile.TemporaryDirectory(prefix="password-display-test-") as directory:
        temporary = Path(directory)
        for name in HEADERS:
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "password_display_mock_idf.h"\n')
        executable = temporary / "test_password_display"
        subprocess.run(
            [
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I" + str(temporary), "-I" + str(TESTS),
                str(TESTS / "test_password_display.cpp"), "-o", str(executable),
            ],
            check=True,
        )
        for scenario in (
            "invalid", "fit", "fit_boundary", "overflow", "tick_wrap", "rotation", "scroll_frames",
            "bitmap_fallback", "bitmap_start_time",
        ):
            subprocess.run([str(executable), scenario], check=True)
        for stage in range(1, 4):
            subprocess.run([str(executable), "allocation_failure", str(stage)], check=True)


if __name__ == "__main__":
    main()
