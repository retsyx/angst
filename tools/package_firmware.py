#!/usr/bin/env python3
"""Package public ESP-IDF build outputs for a separately provisioned USB install."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
import zipfile


BUNDLE_ROOT = "angst-esp32s3-flash"
ESPTOOL_VERSION = "5.4.0"
SECTOR_SIZE = 0x1000
PROVISIONING_OFFSET = 0x9000
PROVISIONING_SIZE = 0x6000
SUPPORTED_SETTINGS = {"flash_size": "16MB", "flash_mode": "dio", "flash_freq": "80m"}
SUPPORTED_PARTITIONS = {
    "nvs": (1, 2, 0x9000, 0x6000),
    "otadata": (1, 0, 0xf000, 0x2000),
    "phy_init": (1, 1, 0x11000, 0x1000),
    "ota_0": (0, 0x10, 0x20000, 0x600000),
    "ota_1": (0, 0x11, 0x620000, 0x600000),
    "matter": (1, 2, 0xc20000, 0x20000),
}
ROLES = {
    "bootloader": "bootloader.bin",
    "partition-table": "partition-table.bin",
    "otadata": "ota_data_initial.bin",
    "app": "angst-esp32s3.bin",
}


def integer(value, description):
    if isinstance(value, bool):
        raise ValueError(f"{description} must be an integer")
    if isinstance(value, str):
        value = int(value, 0)
    if not isinstance(value, int) or value < 0:
        raise ValueError(f"{description} must be a nonnegative integer")
    return value


def read_build_file(build_dir, name):
    if not isinstance(name, str) or not name:
        raise ValueError("Build output filename must be a nonempty string")
    path = (build_dir / name).resolve()
    try:
        path.relative_to(build_dir)
    except ValueError as error:
        raise ValueError("Build outputs must remain inside the selected build directory") from error
    if not path.is_file():
        raise ValueError(f"Missing build output: {name}")
    return path.read_bytes()


def partition_table(data, require_md5):
    if not data or len(data) % 32:
        raise ValueError("Partition table must contain complete 32-byte records")
    partitions = []
    names = set()
    md5_seen = False
    for position in range(0, len(data), 32):
        entry = data[position:position + 32]
        if entry == b"\xff" * 32:
            if any(value != 0xff for value in data[position:]):
                raise ValueError("Partition table has data after its terminator")
            break
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0xebeb:
            if md5_seen or entry[2:16] != b"\xff" * 14:
                raise ValueError("Invalid partition-table checksum record")
            if hashlib.md5(data[:position]).digest() != entry[16:]:
                raise ValueError("Partition-table checksum does not match its entries")
            if any(value != 0xff for value in data[position + 32:]):
                raise ValueError("Partition table has entries after its checksum")
            md5_seen = True
            break
        if magic != 0x50aa:
            raise ValueError("Invalid partition-table record magic")
        _, kind, subtype, offset, size, raw_name, flags = struct.unpack("<HBBII16sI", entry)
        name = raw_name.split(b"\0", 1)[0].decode("utf-8")
        if not name or name in names:
            raise ValueError("Partition names must be nonempty and unique")
        if not size or offset % SECTOR_SIZE or size % SECTOR_SIZE:
            raise ValueError(f"Partition {name} is empty or not flash-sector aligned")
        names.add(name)
        partitions.append({"name": name, "type": kind, "subtype": subtype,
                           "offset": offset, "size": size, "flags": flags})
    if require_md5 and not md5_seen:
        raise ValueError("Configured partition-table checksum is missing")
    if not partitions:
        raise ValueError("Partition table contains no partitions")
    return partitions


def check_ranges(ranges, flash_bytes, description):
    ordered = sorted(ranges, key=lambda item: item["offset"])
    previous_end = 0
    for index, item in enumerate(ordered):
        end = item["offset"] + item["size"]
        if end > flash_bytes:
            raise ValueError(f"{description} exceeds the configured flash size")
        if index and item["offset"] < previous_end:
            raise ValueError(f"{description} overlap")
        previous_end = end


def public_range(partition):
    return {key: partition[key] for key in ("name", "offset", "size")}


def package_firmware(build_dir, output, flasher, readme):
    build_dir = Path(build_dir).resolve()
    output = Path(output)
    args = json.loads(read_build_file(build_dir, "flasher_args.json"))
    config = json.loads(read_build_file(build_dir, "config/sdkconfig.json"))
    if not isinstance(args, dict) or not isinstance(config, dict):
        raise ValueError("ESP-IDF build metadata must be JSON objects")

    settings = args["flash_settings"]
    chip = args["extra_esptool_args"]["chip"]
    if chip != "esp32s3" or chip != config["IDF_TARGET"]:
        raise ValueError("Firmware bundle requires matching ESP32-S3 build metadata")
    if config.get("SECURE_BOOT") or config.get("FLASH_ENCRYPTION_ENABLED"):
        raise ValueError("This public USB bundle does not support secured build outputs")
    for key, config_key in (("flash_size", "ESPTOOLPY_FLASHSIZE"),
                            ("flash_mode", "ESPTOOLPY_FLASHMODE"),
                            ("flash_freq", "ESPTOOLPY_FLASHFREQ")):
        if not isinstance(settings[key], str) or settings[key] != config[config_key]:
            raise ValueError(f"Conflicting {key} in ESP-IDF build metadata")
        if settings[key] != SUPPORTED_SETTINGS[key]:
            raise ValueError(f"Unsupported {key} for the LilyGo flash helper")
    size_match = re.fullmatch(r"([1-9][0-9]*)MB", settings["flash_size"])
    if not size_match:
        raise ValueError("Flash size must be an explicit number of megabytes")
    flash_bytes = int(size_match[1]) * 1024 * 1024

    files = args["flash_files"]
    if not isinstance(files, dict):
        raise ValueError("ESP-IDF flash_files must be a mapping")
    declared = {}
    for offset, filename in files.items():
        address = integer(offset, "Flash file offset")
        if address in declared:
            raise ValueError("Duplicate flash file offset")
        declared[address] = filename

    payloads = {}
    segments = []
    role_offsets = {}
    for role, bundled_name in ROLES.items():
        metadata = args[role]
        offset = integer(metadata["offset"], f"{role} offset")
        filename = metadata["file"]
        if metadata.get("encrypted") not in (None, False, "false"):
            raise ValueError("Encrypted flash segments cannot be distributed in this bundle")
        if offset % SECTOR_SIZE or declared.get(offset) != filename:
            raise ValueError(f"Conflicting or unaligned {role} flash metadata")
        payload = read_build_file(build_dir, filename)
        if not payload:
            raise ValueError(f"Empty {role} output")
        if role in ("bootloader", "app") and payload[0] != 0xe9:
            raise ValueError(f"{role} is not an ESP application image")
        payloads[bundled_name] = payload
        role_offsets[role] = offset
        segments.append({"offset": offset, "file": bundled_name,
                         "sha256": hashlib.sha256(payload).hexdigest(), "size": len(payload)})
    if len(declared) != len(ROLES) or set(declared) != set(role_offsets.values()):
        raise ValueError("Unexpected flash outputs: only bootloader, table, OTA data, and app may be published")
    if role_offsets != {"bootloader": 0, "partition-table": 0x8000, "otadata": 0xf000, "app": 0x20000}:
        raise ValueError("Flash segment placement is unsupported by the LilyGo flash helper")

    table_offset = integer(config["PARTITION_TABLE_OFFSET"], "Configured partition-table offset")
    if role_offsets["partition-table"] != table_offset:
        raise ValueError("Partition-table flash offset does not match sdkconfig")
    if len(payloads["partition-table.bin"]) not in (0xc00, SECTOR_SIZE):
        raise ValueError("Partition-table output must use ESP-IDF's reserved table size")
    if config.get("PARTITION_TABLE_MD5", True) is not True:
        raise ValueError("Flash helper requires a checksummed partition table")
    partitions = partition_table(payloads["partition-table.bin"], True)
    check_ranges(partitions + [{"offset": table_offset, "size": SECTOR_SIZE}],
                 flash_bytes, "Partition ranges")
    by_name = {item["name"]: item for item in partitions}
    if set(by_name) != set(SUPPORTED_PARTITIONS):
        raise ValueError("Partition set is unsupported by the LilyGo flash helper")
    for name, expected in SUPPORTED_PARTITIONS.items():
        actual = by_name[name]
        if tuple(actual[key] for key in ("type", "subtype", "offset", "size")) != expected or actual["flags"]:
            raise ValueError(f"Partition {name} differs from the LilyGo flash helper's protected layout")
    provisioning = by_name["nvs"]
    matter = by_name[config["ESP_MATTER_NVS_PART_NAME"]]
    for item in (provisioning, matter):
        if item["type"] != 1 or item["subtype"] != 2 or item["flags"]:
            raise ValueError("Provisioning and Matter storage must be unencrypted NVS partitions")
    if provisioning is matter:
        raise ValueError("Provisioning and Matter must use separate NVS partitions")
    if (provisioning["offset"], provisioning["size"]) != (PROVISIONING_OFFSET, PROVISIONING_SIZE):
        raise ValueError("Provisioning partition does not match the commissioner's 0x9000/0x6000 contract")

    app_partitions = sorted((item for item in partitions if item["type"] == 0), key=lambda item: item["offset"])
    app_partition = next((item for item in app_partitions if item["offset"] == role_offsets["app"]), None)
    if not app_partition or len(payloads["angst-esp32s3.bin"]) > app_partition["size"]:
        raise ValueError("Application output does not fit its declared application partition")
    ota_partition = next((item for item in partitions if item["type"] == 1 and item["subtype"] == 0), None)
    if (not ota_partition or ota_partition["offset"] != role_offsets["otadata"] or
            len(payloads["ota_data_initial.bin"]) != ota_partition["size"] or
            any(value != 0xff for value in payloads["ota_data_initial.bin"])):
        raise ValueError("Initial OTA data must be erased bytes matching the OTA-data partition")

    segment_ranges = [{"offset": item["offset"],
                       "size": (item["size"] + SECTOR_SIZE - 1) // SECTOR_SIZE * SECTOR_SIZE}
                      for item in segments]
    protected = [public_range(provisioning), public_range(matter)]
    check_ranges(segment_ranges + protected, flash_bytes, "Firmware segments and protected NVS ranges")
    vendor_id = integer(config["DEVICE_VENDOR_ID"], "Attestation vendor ID")
    product_id = integer(config["DEVICE_PRODUCT_ID"], "Attestation product ID")
    if not 0 < vendor_id <= 0xffff or not 0 < product_id <= 0xffff:
        raise ValueError("Attestation vendor and product IDs must be nonzero 16-bit values")
    manifest = {"schema_version": 1, "chip": chip,
                **{key: settings[key] for key in ("flash_size", "flash_mode", "flash_freq")},
                "esptool_version": ESPTOOL_VERSION,
                "vendor_id": vendor_id, "product_id": product_id,
                "provisioning": {key: provisioning[key] for key in ("offset", "size")},
                "matter": {key: matter[key] for key in ("offset", "size")},
                "app_partitions": [public_range(item) for item in app_partitions],
                "protected_ranges": protected,
                "segments": sorted(segments, key=lambda item: item["offset"])}
    payloads["flash-manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    payloads["flash_firmware.py"] = Path(flasher).read_bytes()
    payloads["README.txt"] = Path(readme).read_bytes()
    if not payloads["flash_firmware.py"] or not payloads["README.txt"]:
        raise ValueError("Flash helper and installation instructions must not be empty")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
        with zipfile.ZipFile(temporary, "w") as archive:
            for filename in sorted(payloads):
                member = zipfile.ZipInfo(f"{BUNDLE_ROOT}/{filename}", date_time=(1980, 1, 1, 0, 0, 0))
                member.create_system = 3
                mode = 0o755 if filename == "flash_firmware.py" else 0o644
                member.external_attr = (0o100000 | mode) << 16
                archive.writestr(member, payloads[filename], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return manifest


def main():
    repository = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--flasher", type=Path, default=repository / "tools" / "flash_firmware.py")
    parser.add_argument("--readme", type=Path, default=repository / "tools" / "commissioning" / "README.md")
    arguments = parser.parse_args()
    try:
        manifest = package_firmware(arguments.build_dir, arguments.output, arguments.flasher, arguments.readme)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"Cannot package firmware: {error}\n")
    print(f"Created {arguments.output} with {len(manifest['segments'])} public firmware segments.")


if __name__ == "__main__":
    main()
