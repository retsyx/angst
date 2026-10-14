#!/usr/bin/env python3
"""Run the real animation request bridge with a pthread mutex on the host."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"


def main():
    with tempfile.TemporaryDirectory(prefix="display-animation-bridge-") as directory:
        temporary = Path(directory)
        for name in ("esp_err.h", "freertos/FreeRTOS.h", "freertos/semphr.h"):
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "price_mock_idf.h"\n')
        executable = temporary / "animation_bridge_test"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-D_XOPEN_SOURCE=700",
             "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-pthread",
             "-I" + str(temporary), "-I" + str(TESTS), "-I" + str(ROOT / "main"),
             str(TESTS / "test_animation_test.c"), str(ROOT / "main" / "animation_test.c"),
             "-o", str(executable)], check=True)
        for scenario in ("startup", "latest_and_failures", "status_lifecycle", "concurrent"):
            subprocess.run([str(executable), scenario], check=True)


if __name__ == "__main__":
    main()
