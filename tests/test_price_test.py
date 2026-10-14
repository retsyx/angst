#!/usr/bin/env python3
"""Test the real price controller/math with a pthread mutex and fake clock/RNG."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
SCENARIOS = (
    "uninitialized", "initialization_failure", "fallback", "cumulative",
    "restoration_deadline", "extended_deadline", "request_capacity", "take_capacity",
    "no_live_expiry", "invalid_live", "lock_failure", "concurrent",
)


def main():
    with tempfile.TemporaryDirectory(prefix="display-price-controller-") as directory:
        temporary = Path(directory)
        for name in ("esp_err.h", "esp_random.h", "esp_timer.h", "freertos/FreeRTOS.h",
                     "freertos/semphr.h", "freertos/queue.h"):
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "price_mock_idf.h"\n')
        executable = temporary / "price_test"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-pthread", "-I" + str(temporary),
             "-I" + str(TESTS), "-I" + str(ROOT / "main"), str(TESTS / "test_price_test.c"),
             str(ROOT / "main" / "price_test.c"), str(ROOT / "main" / "price_test_math.c"),
             "-o", str(executable)], check=True)
        for scenario in SCENARIOS:
            subprocess.run([str(executable), scenario], check=True)
        # Repeat concurrency with fresh controller state and fresh scheduling.
        for _ in range(2):
            subprocess.run([str(executable), "concurrent"], check=True)


if __name__ == "__main__":
    main()
