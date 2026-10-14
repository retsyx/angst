#!/usr/bin/env python3
"""Check read-only Matter first-install detection with mocked ESP-IDF APIs."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="matter-bootstrap-test-") as directory:
        temporary = Path(directory)
        for name in ("esp_err.h", "esp_app_format.h", "esp_ota_ops.h", "esp_partition.h"):
            (temporary / name).write_text('#include "matter_bootstrap_mock_idf.h"\n')
        executable = temporary / "test_bootstrap"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I" + str(temporary),
                "-I" + str(TESTS),
                str(TESTS / "test_matter_bootstrap.c"),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
