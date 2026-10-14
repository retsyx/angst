#!/usr/bin/env python3
"""Run actual Matter application callbacks against focused host API mocks."""

from pathlib import Path
import os
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent
HEADERS = (
    "esp_err.h", "matter_identity.h", "esp_log.h", "esp_timer.h", "freertos/FreeRTOS.h",
    "freertos/event_groups.h",
    "esp_matter.h", "esp_matter_console.h", "esp_matter_providers.h",
    "app/server/CommissioningWindowManager.h", "app/server/Server.h", "platform/CHIPDeviceLayer.h",
    "app/persistence/AttributePersistence.h", "app/persistence/DefaultAttributePersistenceProvider.h",
    "platform/CHIPDeviceError.h", "platform/KvsPersistentStorageDelegate.h",
)


def main():
    with tempfile.TemporaryDirectory(prefix="matter-display-test-") as directory:
        temporary = Path(directory)
        for name in HEADERS:
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "matter_mock_idf.h"\n')
        executable = temporary / "test_matter_display"
        compiler = [
            os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I" + str(temporary), "-I" + str(TESTS),
        ]
        source = str(TESTS / "test_matter_display.cpp")
        subprocess.run(
            compiler + [source, "-o", str(executable)],
            check=True,
        )
        incompatible_mdns = subprocess.run(
            compiler + ["-DCONFIG_USE_MINIMAL_MDNS=1", "-fsyntax-only", source],
            capture_output=True,
            text=True,
        )
        expected_diagnostic = (
            "Disable USE_MINIMAL_MDNS so Matter and the web service share the Espressif mDNS responder"
        )
        if incompatible_mdns.returncode == 0 or expected_diagnostic not in incompatible_mdns.stderr:
            raise RuntimeError(
                "Matter must reject the competing minimal mDNS responder with an explicit diagnostic:\n"
                + incompatible_mdns.stdout + incompatible_mdns.stderr
            )
        print("Competing minimal mDNS configuration rejected")
        for scenario in (
            "fresh", "restored", "null", "empty_label", "saved_label", "saved_unique_id",
            "label_read_error", "unique_id_read_error",
            "ready_while_waiting", "server_never_ready", "unrelated_startup_event",
            "sdk_start_error", "sdk_error_after_ready", "event_group_allocation_error",
            "sdk_reset", "existing_record_overrides", "provisioning_corrupt", "provisioning_error",
            "seed_error", "bootstrap_error", "existing_identity_absent",
            "existing_identity_absent_after_reset", "existing_id_even_fresh",
        ):
            subprocess.run([str(executable), scenario], check=True)


if __name__ == "__main__":
    main()
