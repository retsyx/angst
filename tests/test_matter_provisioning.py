#!/usr/bin/env python3
"""Exercise immutable Matter identity storage with the actual production module."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="matter-provisioning-test-") as directory:
        temporary = Path(directory)
        for name in ("esp_err.h", "nvs.h"):
            (temporary / name).write_text('#include "matter_provisioning_mock_idf.h"\n')
        executable = temporary / "test_matter_provisioning"
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
                str(TESTS / "test_matter_provisioning.c"),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
