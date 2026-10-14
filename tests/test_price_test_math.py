#!/usr/bin/env python3
"""Check the pure C price generator on the host with address/UB sanitizers."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="t-display-price-test-") as directory:
        executable = Path(directory) / "price_test_math"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-fsanitize=address,undefined",
                "-I" + str(ROOT / "main"),
                str(ROOT / "tests" / "test_price_test_math.c"),
                str(ROOT / "main" / "price_test_math.c"),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
