#!/usr/bin/env python3
"""Check runtime price-source persistence and worker behavior with the SDK URL parser."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent
HEADERS = (
    "esp_err.h", "esp_crt_bundle.h", "esp_event.h", "esp_http_client.h",
    "esp_log.h", "esp_netif.h", "esp_netif_sntp.h", "esp_wifi.h", "mdns.h",
    "nvs.h", "sdkconfig.h", "freertos/FreeRTOS.h", "freertos/queue.h",
    "freertos/event_groups.h", "freertos/task.h", "freertos/semphr.h",
)


def parser_directory():
    if override := os.environ.get("SERVICE_HTTP_PARSER_DIR"):
        return Path(override)
    if idf := os.environ.get("IDF_PATH"):
        return Path(idf) / "components" / "http_parser"
    installed = sorted(Path.home().glob(".espressif/*/esp-idf/components/http_parser"))
    if not installed:
        raise RuntimeError("Set IDF_PATH to an ESP-IDF checkout to use its actual HTTP URL parser")
    return installed[-1]


def main():
    parser = parser_directory()
    with tempfile.TemporaryDirectory(prefix="display-service-url-test-") as directory:
        temporary = Path(directory)
        for name in HEADERS:
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "service_mock_idf.h"\n')
        executable = temporary / "service_url_test"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
             "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-variable", "-fsanitize=address,undefined",
             "-I" + str(temporary), "-I" + str(TESTS), "-I" + str(TESTS.parent / "main"),
             "-I" + str(parser), str(TESTS / "test_service_url.c"),
             str(TESTS.parent / "main" / "service.c"), str(parser / "http_parser.c"),
             "-o", str(executable)],
            check=True,
        )
        for scenario in (
            "empty", "restore", "invalid-stored", "oversized-stored", "corrupt-stored",
            "load-failure", "open-failure", "allocation-failure", "validation",
            "save-failure", "commit-failure", "persist", "worker", "changed-fetch",
            "disable-fetch", "poll-wakeup",
        ):
            subprocess.run([str(executable), scenario], check=True)
        print("Price URL host checks passed: empty defaults, persisted sources, validation, "
              "failed saves, immediate rescheduling, and stale-response suppression.")


if __name__ == "__main__":
    main()
