#!/usr/bin/env python3
"""Exercise real LED routing code against GPIO-matrix and PWM host mocks."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="led-lighting-test-") as directory:
        temporary = Path(directory)
        for name in ("esp_err.h", "driver/gpio.h", "driver/ledc.h"):
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "lighting_mock_idf.h"\n')
        executable = temporary / "test_lighting"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I" + str(temporary), "-I" + str(TESTS),
            str(TESTS / "test_led_lighting.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
