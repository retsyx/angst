#!/usr/bin/env python3
"""Run the native web handlers and their actual generated browser script on the host."""

from pathlib import Path
import os
import re
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent
HEADERS = (
    "sdkconfig.h",
    "esp_err.h",
    "esp_app_format.h",
    "esp_http_server.h",
    "esp_log.h",
    "esp_ota_ops.h",
    "esp_system.h",
    "lvgl.h",
    "freertos/FreeRTOS.h",
    "freertos/task.h",
    "freertos/queue.h",
)


def main():
    with tempfile.TemporaryDirectory(prefix="display-web-test-") as directory:
        temporary = Path(directory)
        for name in HEADERS:
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "web_mock_idf.h"\n')
        executable = temporary / "web_test"
        page = temporary / "page.html"
        source_flag = []
        if source := os.environ.get("WEB_SERVER_SOURCE"):
            source_flag = ['-DOTA_SERVER_SOURCE="' + str(Path(source).resolve()) + '"']
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-variable",
                "-fsanitize=address,undefined",
                "-I" + str(temporary),
                "-I" + str(TESTS),
                *source_flag,
                str(TESTS / "test_web_server.c"),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable), str(page)], check=True)
        script = re.search(r"<script>(.*?)</script>", page.read_text(), re.DOTALL)
        assert script, "Generated page has no script"
        javascript = temporary / "page.js"
        javascript.write_text(script.group(1))
        subprocess.run(["node", "--check", str(javascript)], check=True)
        subprocess.run(["node", str(TESTS / "test_web_page.js"), str(javascript)], check=True)


if __name__ == "__main__":
    main()
