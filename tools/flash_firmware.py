#!/usr/bin/env python3
"""Install a release bundle and a private identity on a fresh Angst board."""

import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unicodedata
import zlib


ESPTOOL_VERSION = "5.4.0"
FLASH_SIZE = 16 * 1024 * 1024
NVS_OFFSET, NVS_SIZE = 0x9000, 0x6000
SEGMENTS = {"bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "angst-esp32s3.bin"}
PARTITIONS = {
    "nvs": (1, 2, 0x9000, 0x6000),
    "otadata": (1, 0, 0xF000, 0x2000),
    "phy_init": (1, 1, 0x11000, 0x1000),
    "ota_0": (0, 0x10, 0x20000, 0x600000),
    "ota_1": (0, 0x11, 0x620000, 0x600000),
    "matter": (1, 2, 0xC20000, 0x20000),
}


class InstallError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise InstallError(message)


def number(value):
    return type(value) is int and value >= 0


def partition_table(data):
    """Read the actual ESP-IDF table, including its MD5 record."""
    require(len(data) in (0xC00, 0x1000), "Partition table must fit its reserved flash sector.")
    entries = {}
    checked = False
    for offset in range(0, len(data), 32):
        row = data[offset:offset + 32]
        magic = struct.unpack_from("<H", row)[0]
        if magic == 0xEBEB:
            require(row[2:16] == b"\xff" * 14 and
                    row[16:] == hashlib.md5(data[:offset]).digest(), "Partition table checksum failed.")
            require(data[offset + 32:] == b"\xff" * (len(data) - offset - 32),
                    "Unexpected data after partition table checksum.")
            checked = True
            break
        require(magic == 0x50AA, "Invalid partition table entry.")
        _, kind, subtype, address, size, label, flags = struct.unpack("<HBBII16sI", row)
        try:
            name = label.split(b"\0", 1)[0].decode("ascii")
        except UnicodeError as error:
            raise InstallError("Invalid partition label.") from error
        require(name and name not in entries and address % 4096 == 0 and size > 0 and
                size % 4096 == 0 and address + size <= FLASH_SIZE and flags == 0,
                "Unsupported partition layout.")
        entries[name] = {"name": name, "offset": address, "size": size,
                         "type": kind, "subtype": subtype}
    require(checked, "Partition table has no checksum.")
    ordered = sorted(entries.values(), key=lambda item: item["offset"])
    require(all(a["offset"] + a["size"] <= b["offset"] for a, b in zip(ordered, ordered[1:])),
            "Partition ranges overlap.")
    return entries


def location(partition):
    return {key: partition[key] for key in ("offset", "size")}


def load_bundle(manifest_path):
    manifest_path = Path(manifest_path)
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, ValueError) as error:
        raise InstallError("Cannot read flash-manifest.json.") from error
    require(isinstance(manifest, dict) and type(manifest.get("schema_version")) is int and
            manifest["schema_version"] == 1,
            "Unsupported flash manifest version.")
    require(manifest.get("chip") == "esp32s3" and manifest.get("flash_size") == "16MB" and
            manifest.get("flash_mode") == "dio" and manifest.get("flash_freq") == "80m" and
            manifest.get("esptool_version") == ESPTOOL_VERSION, "Unsupported board or flash settings.")
    for key in ("vendor_id", "product_id"):
        require(number(manifest.get(key)) and 0 < manifest[key] <= 0xFFFF,
                "Manifest is missing the firmware's Matter vendor/product IDs.")
    segments = manifest.get("segments")
    require(isinstance(segments, list) and len(segments) == 4 and
            all(isinstance(item, dict) and isinstance(item.get("file"), str) for item in segments) and
            {item["file"] for item in segments} == SEGMENTS, "Incomplete firmware bundle.")
    images = {}
    for item in segments:
        name, offset, size = item["file"], item.get("offset"), item.get("size")
        require(number(offset) and offset % 4096 == 0 and number(size) and size > 0 and
                offset + size <= FLASH_SIZE, "Invalid firmware segment range.")
        try:
            data = (manifest_path.parent / name).read_bytes()
        except OSError as error:
            raise InstallError("Missing firmware file: " + name) from error
        require(len(data) == size and hashlib.sha256(data).hexdigest() == item.get("sha256"),
                "Firmware size/checksum failed: " + name)
        images[name] = (offset, data)
    table = partition_table(images["partition-table.bin"][1])
    require(set(table) == set(PARTITIONS), "Unsupported firmware partition set.")
    for name, expected in PARTITIONS.items():
        actual = table[name]
        require(tuple(actual[key] for key in ("type", "subtype", "offset", "size")) == expected,
                "Unsupported firmware partition layout: " + name)
    require(location(table["nvs"]) == {"offset": NVS_OFFSET, "size": NVS_SIZE} and
            manifest.get("provisioning") == location(table["nvs"]) and
            manifest.get("matter") == location(table["matter"]), "Provisioning layout mismatch.")
    protected = [{"name": name, **location(table[name])} for name in ("nvs", "matter")]
    require(manifest.get("protected_ranges") == protected, "Incomplete protected storage ranges.")
    apps = [{"name": name, **location(table[name])} for name in ("ota_0", "ota_1")]
    require(manifest.get("app_partitions") == apps, "Application partition layout mismatch.")
    require(images["bootloader.bin"][0] == 0 and len(images["bootloader.bin"][1]) <= 0x8000 and
            images["partition-table.bin"][0] == 0x8000, "Invalid bootloader/partition-table placement.")
    for filename, name in (("ota_data_initial.bin", "otadata"), ("angst-esp32s3.bin", "ota_0")):
        offset, data = images[filename]
        require(offset == table[name]["offset"] and len(data) <= table[name]["size"],
                "Firmware does not fit its partition: " + filename)
    require(images["ota_data_initial.bin"][1] == b"\xff" * table["otadata"]["size"],
            "Initial OTA data is not erased.")
    for name in ("bootloader.bin", "angst-esp32s3.bin"):
        require(images[name][1][0] == 0xE9, "Invalid ESP application/bootloader image: " + name)
    # Account for whole-sector erasure, not just bytes in each input file.
    regions = [(offset, offset + ((len(data) + 4095) // 4096) * 4096)
               for offset, data in images.values()]
    regions += [(item["offset"], item["offset"] + item["size"]) for item in protected]
    regions.sort()
    require(all(a[1] <= b[0] for a, b in zip(regions, regions[1:])), "Flash regions overlap protected storage.")
    return manifest, images


def validate_provisioning(data, manifest):
    """Accept only the commissioner's complete one-blob NVS V2 image."""
    require(len(data) == NVS_SIZE, "Provisioning image must be 24576 bytes; use angst-commissioner.")
    require(data[:9] == struct.pack("<IIB", 0xFFFFFFFE, 0, 0xFE) and
            data[9:28] == b"\xff" * 19 and
            struct.unpack_from("<I", data, 28)[0] == zlib.crc32(data[4:28], 0xFFFFFFFF),
            "Invalid NVS page header.")
    expected_bitmap = bytearray(b"\xff" * 32)
    for index in range(10):
        expected_bitmap[index // 4] &= ~(1 << ((index % 4) * 2))
    require(data[32:64] == expected_bitmap and data[384:] == b"\xff" * (NVS_SIZE - 384),
            "Provisioning image has unexpected entries or data.")
    for index, header, key in ((0, bytes((0, 1, 1, 255)), b"matter_prov"),
                               (1, bytes((1, 0x42, 8, 0)), b"identity"),
                               (9, bytes((1, 0x48, 1, 255)), b"identity")):
        entry = data[64 + index * 32:96 + index * 32]
        require(entry[:4] == header and entry[8:24] == key.ljust(16, b"\0") and
                struct.unpack_from("<I", entry, 4)[0] ==
                zlib.crc32(entry[:4] + entry[8:], 0xFFFFFFFF), "Invalid NVS entry.")
    require(data[88:96] == b"\x01" + b"\xff" * 7 and
            data[120:124] == struct.pack("<HH", 200, 0xFFFF) and
            data[376:384] == struct.pack("<IBB", 200, 1, 0) + b"\xff\xff" and
            data[328:352] == b"\xff" * 24, "Invalid NVS blob structure.")
    record = data[128:328]
    require(struct.unpack_from("<I", data, 124)[0] == zlib.crc32(record, 0xFFFFFFFF),
            "Provisioning blob checksum failed.")
    require(record[:8] == b"AMPI\x01\0\xc8\0" and
            struct.unpack_from("<I", record, 196)[0] == zlib.crc32(record[:196]),
            "Invalid Matter identity record.")
    discriminator, iterations, vendor, product = struct.unpack_from("<HIHH", record, 8)
    require(discriminator <= 4095 and 1000 <= iterations <= 100000, "Invalid Matter commissioning parameters.")
    require(vendor == manifest["vendor_id"] and product == manifest["product_id"],
            "Provisioning vendor/product IDs do not match this firmware's attestation settings.")
    length = record[18]
    require(1 <= length <= 32 and record[19 + length:51] == b"\0" * (32 - length),
            "Invalid provisioned serial number.")
    try:
        serial = record[19:19 + length].decode("utf-8")
    except UnicodeError as error:
        raise InstallError("Invalid provisioned serial number.") from error
    require(not any(unicodedata.category(char) == "Cc" for char in serial),
            "Invalid provisioned serial number.")


def install(manifest_path, provisioning_path, port, baud=460800, runner=subprocess.run):
    manifest, images = load_bundle(manifest_path)
    try:
        provisioning = Path(provisioning_path).read_bytes()
    except OSError as error:
        raise InstallError("Cannot read provisioning-nvs.bin; generate this board's identity first.") from error
    validate_provisioning(provisioning, manifest)
    try:
        installed_version = importlib.metadata.version("esptool")
    except importlib.metadata.PackageNotFoundError as error:
        raise InstallError("Install esptool first: python -m pip install esptool==" + ESPTOOL_VERSION) from error
    require(installed_version == ESPTOOL_VERSION, "Use esptool==" + ESPTOOL_VERSION + " for this bundle.")
    require(isinstance(port, str) and port and number(baud) and baud > 0, "Specify a serial port and positive baud rate.")
    command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port, "--baud", str(baud)]
    with tempfile.TemporaryDirectory(prefix="angst-install-") as temporary:
        directory = Path(temporary)
        # Freeze verified inputs before opening the serial port.
        for name, (_, content) in images.items():
            path = directory / name
            path.write_bytes(content)
            path.chmod(0o600)
        provisioned = directory / "provisioning-nvs.bin"
        provisioned.write_bytes(provisioning)
        provisioned.chmod(0o600)
        before = "default-reset"
        for item in manifest["protected_ranges"]:
            path = directory / (item["name"] + "-existing.bin")
            runner(command + ["--before", before, "--after", "no-reset", "read-flash", "--no-progress",
                              hex(item["offset"]), hex(item["size"]), str(path)], check=True)
            try:
                existing = path.read_bytes()
            except OSError as error:
                raise InstallError("Protected storage could not be read; firmware was not written.") from error
            require(len(existing) == item["size"], "Protected storage read was incomplete; firmware was not written.")
            require(existing == b"\xff" * item["size"],
                    "Saved data exists in " + item["name"] + "; installation refused. "
                    "Use browser OTA for an existing Angst display. Nothing was written; press RESET to restart it.")
            # Keep the CPU in the ROM loader until all checks and writes finish;
            # the old app must not initialize NVS between checking and writing.
            before = "no-reset"
        writes = [(offset, str(directory / name)) for name, (offset, _) in images.items()]
        writes.append((NVS_OFFSET, str(provisioned)))
        arguments = ["--before", "no-reset", "--after", "hard-reset", "write-flash",
                     "--flash-mode", manifest["flash_mode"], "--flash-size", manifest["flash_size"],
                     "--flash-freq", manifest["flash_freq"]]
        for offset, path in sorted(writes):
            arguments.extend((hex(offset), path))
        runner(command + arguments, check=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USB serial port, for example COM3 or /dev/ttyACM0")
    parser.add_argument("--provisioning", required=True, type=Path, help="This board's private provisioning-nvs.bin")
    parser.add_argument("--baud", type=int, default=460800, help="Serial baud rate (default: 460800)")
    arguments = parser.parse_args(argv)
    try:
        install(Path(__file__).resolve().with_name("flash-manifest.json"), arguments.provisioning,
                arguments.port, arguments.baud)
    except (InstallError, OSError, subprocess.CalledProcessError) as error:
        print("Installation stopped: " + str(error), file=sys.stderr)
        return 1
    print("Firmware and identity installed. Pair with this board's qrcode.png or onboarding.txt.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
