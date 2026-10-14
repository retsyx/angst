#!/usr/bin/env python3
"""Compare native Go provisioning NVS with ESP-IDF and the firmware decoder.

Run after sourcing the pinned ESP-IDF environment. All generated credentials
and intermediate artifacts stay in a private, automatically removed directory.
"""

import base64
import csv
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent
PARTITION_SIZE = 0x6000
RECORD_SIZE = 200

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "matter_provisioning_mock_idf.h"
#include "matter_provisioning.h"
#include "matter_identity.h"

static unsigned char record[200];
static unsigned opens, reads, closes, handles;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "matter_prov"));
    assert(mode == NVS_READONLY && handle && !handles);
    ++opens;
    ++handles;
    *handle = 7;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle == 7 && handles == 1 && !strcmp(key, "identity"));
    assert(value && size && *size >= sizeof(record));
    memcpy(value, record, sizeof(record));
    *size = sizeof(record);
    ++reads;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    (void)handle; (void)key; (void)value; (void)size;
    assert(!"Loading the exported identity must never write NVS");
    return ESP_FAIL;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    assert(!"Loading the exported identity must never commit NVS");
    return ESP_FAIL;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 7 && handles == 1);
    --handles;
    ++closes;
}

static void verify_identity(const matter_provisioning_identity_t *identity)
{
    assert(identity->discriminator == DISPLAY_MATTER_DISCRIMINATOR);
    assert(identity->iterations == DISPLAY_MATTER_SPAKE2P_ITERATIONS);
    assert(identity->vendor_id == DISPLAY_MATTER_VENDOR_ID);
    assert(identity->product_id == DISPLAY_MATTER_PRODUCT_ID);
    assert(!strcmp(identity->serial_number, DISPLAY_MATTER_SERIAL_NUMBER));
    assert(sizeof(identity->salt) == sizeof(DISPLAY_MATTER_SPAKE2P_SALT));
    assert(!memcmp(identity->salt, DISPLAY_MATTER_SPAKE2P_SALT, sizeof(identity->salt)));
    assert(sizeof(identity->verifier) == sizeof(DISPLAY_MATTER_SPAKE2P_VERIFIER));
    assert(!memcmp(identity->verifier, DISPLAY_MATTER_SPAKE2P_VERIFIER, sizeof(identity->verifier)));
    assert(sizeof(identity->rotating_id) == sizeof(DISPLAY_MATTER_ROTATING_ID));
    assert(!memcmp(identity->rotating_id, DISPLAY_MATTER_ROTATING_ID, sizeof(identity->rotating_id)));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *input = fopen(argv[1], "rb");
    assert(input && fread(record, 1, sizeof(record), input) == sizeof(record));
    assert(fgetc(input) == EOF && !ferror(input));
    assert(fclose(input) == 0);
    matter_provisioning_identity_t identity;
    assert(matter_provisioning_load(NULL, false, &identity) == ESP_OK);
    verify_identity(&identity);
    assert(opens == 1 && reads == 1 && closes == 1 && !handles);
    // A saved exported record wins even when seeding is permitted.
    assert(matter_provisioning_load(NULL, true, &identity) == ESP_OK);
    verify_identity(&identity);
    assert(opens == 2 && reads == 2 && closes == 2 && !handles);
    unsigned char before[sizeof(identity)];
    memset(&identity, 0x5a, sizeof(identity));
    memcpy(before, &identity, sizeof(before));
    record[sizeof(record) - 1] ^= 1;
    assert(matter_provisioning_load(NULL, true, &identity) == ESP_ERR_INVALID_RESPONSE);
    assert(!memcmp(before, &identity, sizeof(before)));
    assert(opens == 3 && reads == 3 && closes == 3 && !handles);
    return 0;
}
"""


def independent_record(device):
    """Encode the documented portable AMPI record independently of Go/C."""
    serial = device["serial_number"].encode("utf-8")
    record = bytearray(RECORD_SIZE)
    record[:4] = b"AMPI"
    struct.pack_into(
        "<HHHIHHB", record, 4,
        1, RECORD_SIZE, device["discriminator"], device["spake2p_iterations"],
        device["vendor_id"], device["product_id"], len(serial),
    )
    record[19:19 + len(serial)] = serial
    record[51:83] = base64.b64decode(device["spake2p_salt"], validate=True)
    record[83:180] = base64.b64decode(device["spake2p_verifier"], validate=True)
    record[180:196] = base64.b64decode(device["rotating_id"], validate=True)
    struct.pack_into("<I", record, 196, zlib.crc32(record[:196]))
    if len(record) != RECORD_SIZE:
        raise RuntimeError("independent AMPI record has an invalid size")
    return bytes(record)


def checked_run(command, **kwargs):
    # The commissioner prints pairing credentials on stdout. Keep all child
    # output captured, including on failure, so CI logs never publish them.
    result = subprocess.run(command, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{Path(command[0]).name} exited with status {result.returncode}; child output withheld")


def check_fixture(temporary, name, args, generator, go_environment):
    fixture = temporary / name
    fixture.mkdir(mode=0o700)
    generated = fixture / "generated"
    command = [
        "go", "-C", str(ROOT / "tools/commissioning"), "run", "-mod=vendor", ".",
        "--out", str(generated), *args,
    ]
    checked_run(command, env=go_environment)
    identity_json = (generated / "identity.json").read_bytes()
    device = json.loads(identity_json)
    record = independent_record(device)
    record_path = fixture / "reference-record.bin"
    record_path.write_bytes(record)
    csv_path = fixture / "reference.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("key", "type", "encoding", "value"))
        writer.writerow(("matter_prov", "namespace", "", ""))
        writer.writerow(("identity", "file", "binary", str(record_path)))
    official_path = fixture / "official-nvs.bin"
    checked_run([
        sys.executable, str(generator), "generate", str(csv_path), str(official_path),
        hex(PARTITION_SIZE), "--version", "2",
    ])
    native_path = generated / "provisioning-nvs.bin"
    native = native_path.read_bytes()
    if len(native) != PARTITION_SIZE or native != official_path.read_bytes():
        raise RuntimeError("native Go NVS does not match the official ESP-IDF generator byte for byte")
    if native_path.stat().st_mode & 0o777 != 0o600:
        raise RuntimeError("native Go provisioning image must have private 0600 permissions")
    checked_run(command, env=go_environment)
    if identity_json != (generated / "identity.json").read_bytes() or native != native_path.read_bytes():
        raise RuntimeError("rerunning the commissioner changed the identity or provisioning image")

    # Read the Go export's data entry payload, rather than the independent copy,
    # and give it to the actual production loader through a read-only NVS mock.
    exported_record = native[128:128 + RECORD_SIZE]
    if exported_record != record:
        raise RuntimeError("native NVS blob does not contain the expected AMPI record")
    exported_path = fixture / "exported-record.bin"
    exported_path.write_bytes(exported_record)
    for header in ("esp_err.h", "nvs.h"):
        (fixture / header).write_text('#include "matter_provisioning_mock_idf.h"\n')
    harness = fixture / "load_exported_identity.c"
    harness.write_text(HARNESS)
    executable = fixture / "load_exported_identity"
    checked_run([
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I" + str(fixture), "-I" + str(TESTS),
        "-I" + str(generated), "-I" + str(ROOT / "main"), str(harness),
        str(ROOT / "main/matter_provisioning.c"), "-o", str(executable),
    ])
    checked_run([str(executable), str(exported_path)])


def main():
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise RuntimeError("IDF_PATH is required; source the pinned ESP-IDF export.sh before this test")
    generator = Path(idf_path) / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    if not generator.is_file():
        raise RuntimeError("IDF_PATH does not contain the official NVS partition generator")
    original_umask = os.umask(0o077)
    try:
        with tempfile.TemporaryDirectory(prefix="provisioning-nvs-compatibility-") as directory:
            temporary = Path(directory)
            go_environment = dict(os.environ)
            go_environment.update({
                "GOCACHE": str(temporary / "go-cache"),
                "GOMODCACHE": str(temporary / "go-mod-cache"),
                "GOPROXY": "off", "GOSUMDB": "off",
            })
            check_fixture(temporary, "random-identity", [], generator, go_environment)
            check_fixture(
                temporary, "maximum-utf8-serial", ["--serial-number", "é" * 16],
                generator, go_environment,
            )
    finally:
        os.umask(original_umask)
    print("Provisioning NVS compatibility passed: official ESP-IDF images match; firmware loads every field read-only with ASAN/UBSAN.")


if __name__ == "__main__":
    main()
