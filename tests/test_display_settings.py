#!/usr/bin/env python3
"""Verify independent animation preferences, persistence, and writer isolation."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="display-settings-test-") as directory:
        temporary = Path(directory)
        for name in (
            "esp_err.h", "esp_log.h", "nvs.h",
            "freertos/FreeRTOS.h", "freertos/semphr.h",
        ):
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "settings_mock_idf.h"\n')
        executable = temporary / "test_settings"
        subprocess.run(
            [
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-variable", "-pthread",
                "-fsanitize=address,undefined", "-I" + str(temporary),
                "-I" + str(TESTS), str(TESTS / "test_display_settings.c"),
                "-o", str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True, timeout=15)
        for animation in (0, 1):
            for stick_figure in (0, 1):
                arguments = [str(temporary / "nvs.bin"), str(animation), str(stick_figure)]
                subprocess.run([str(executable), "save", *arguments], check=True)
                subprocess.run([str(executable), "load", *arguments], check=True)
        print("All four animation preference combinations survived a fresh process.", flush=True)


if __name__ == "__main__":
    main()
