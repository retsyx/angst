#!/usr/bin/env python3
"""Check configurable lighting GPIO validation, persistence and concurrency."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix="lighting-settings-test-") as directory:
        temporary = Path(directory)
        for name in (
            "esp_err.h", "esp_log.h", "nvs.h", "soc/soc_caps.h",
            "freertos/FreeRTOS.h", "freertos/semphr.h",
        ):
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "lighting_settings_mock_idf.h"\n')
        executable = temporary / "test_settings"
        subprocess.run(
            [
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-variable", "-pthread",
                "-fsanitize=address,undefined", "-I" + str(temporary),
                "-I" + str(TESTS), str(TESTS / "test_lighting_settings.c"),
                "-o", str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True, timeout=15)
        for gpios in ("45,46", "1", "1,10,39,45,46"):
            arguments = [str(temporary / "nvs.bin"), gpios]
            subprocess.run([str(executable), "save", *arguments], check=True)
            subprocess.run([str(executable), "load", *arguments], check=True)
        print("Lighting GPIO lists survived fresh-process restart.", flush=True)


if __name__ == "__main__":
    main()
