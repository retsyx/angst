#!/usr/bin/env python3
"""Check random password generation and persistence with mocked ESP-IDF APIs."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="upgrade-password-test-") as directory:
        temporary = Path(directory)
        for name in ("esp_err.h", "esp_random.h", "nvs.h"):
            (temporary / name).write_text('#include "password_mock_idf.h"\n')
        executable = temporary / "test_password"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                # A stale build definition must never become a password fallback.
                '-DCONFIG_DISPLAY_OTA_PASSWORD="must-not-be-used"',
                "-I" + str(temporary),
                "-I" + str(TESTS),
                str(TESTS / "test_upgrade_password.c"),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
