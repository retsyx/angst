#!/usr/bin/env python3
"""Check panel brightness, task ownership, errors, and LCD IO ordering on the host."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent
HEADERS = (
    "esp_err.h", "lvgl.h", "driver/gpio.h", "driver/spi_master.h",
    "esp_check.h", "esp_heap_caps.h", "esp_lcd_panel_io.h", "esp_log.h",
    "freertos/FreeRTOS.h", "freertos/task.h",
)


def main():
    with tempfile.TemporaryDirectory(prefix="display-brightness-test-") as directory:
        temporary = Path(directory)
        for name in HEADERS:
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "display_mock_idf.h"\n')
        executable = temporary / "test_brightness"
        subprocess.run(
            [
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I" + str(temporary), "-I" + str(TESTS),
                str(TESTS / "test_display_brightness.c"), "-o", str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
