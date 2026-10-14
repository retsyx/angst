#!/usr/bin/env python3
"""Exercise USB installation entirely with public fixtures and fake esptool calls."""

import base64
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zlib


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("angst_flash_firmware", ROOT / "tools/flash_firmware.py")
flasher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(flasher)

# Public synthetic fixture, independently produced by the official ESP-IDF NVS
# generator. Matches tools/commissioning/nvs_test.go, and contains no real keys.
PROVISIONING = zlib.decompress(base64.b64decode(
    "eNrt0MdSk2EYxfH3o4bee0e6BgOo9JIAUgOho6CIECBAKOGj9x4FlA2X4IY7gCtwmGGBa++AFRsWLngNM+wycAHM/7c788ycmfPcSSmFw510tqM9Pzs9/SefIhRF/r0+MFv7VdVs65uyTc7d1wnl4a4YNOI23G6wDJonVIu6KB78FlKe2C8i9EZTneJIf9wC04W4kWIjoKmzTVvZbDTp27U6nS5HOFFcXN3cPTw1Xt4+vn7+AYFBwSGhYeERkVHRMbFx8QmJSckvUlLT0jMys16+0ma/1uXk5r15+y6/oLCouKS0rLxCb6isqn5fU1tX39BobGo2tbS2tXd0dnV/+NjT++lz35f+rwOD5qHhEcvo2Lh1YnJq2jajzs7NLywuLa+srq1vbG5t7+zu7du/fT84PPrx8/jy11XYY/9RahVZnenp67zfsUNIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMAz8R/y385H"
))

PARTITIONS = [
    ("nvs", 1, 2, 0x9000, 0x6000),
    ("otadata", 1, 0, 0xF000, 0x2000),
    ("phy_init", 1, 1, 0x11000, 0x1000),
    ("ota_0", 0, 0x10, 0x20000, 0x600000),
    ("ota_1", 0, 0x11, 0x620000, 0x600000),
    ("matter", 1, 2, 0xC20000, 0x20000),
]


def make_partition_table(partitions, flags=0):
    rows = b"".join(
        struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size,
                    name.encode("ascii").ljust(16, b"\0"), flags)
        for name, kind, subtype, offset, size in partitions
    )
    checksum = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(rows).digest()
    return (rows + checksum).ljust(0xC00, b"\xff")


def record_mutation(change):
    data = bytearray(PROVISIONING)
    record = bytearray(data[128:328])
    change(record)
    struct.pack_into("<I", record, 196, zlib.crc32(record[:196]))
    data[128:328] = record
    struct.pack_into("<I", data, 124, zlib.crc32(record, 0xFFFFFFFF))
    struct.pack_into("<I", data, 100, zlib.crc32(data[96:100] + data[104:128], 0xFFFFFFFF))
    return bytes(data)


class FakeEsptool:
    def __init__(self, responses=None, before_first_read=None, fail_write=False):
        self.responses = responses or {}
        self.before_first_read = before_first_read
        self.fail_write = fail_write
        self.calls = []
        self.written = []
        self.staging = None

    def __call__(self, command, **kwargs):
        assert kwargs == {"check": True}
        assert command[:3] == [flasher.sys.executable, "-m", "esptool"]
        assert command[command.index("--port") + 1] == "FAKE-USB-PORT"
        assert command[command.index("--chip") + 1] == "esp32s3"
        assert command[command.index("--baud") + 1] == "460800"
        self.calls.append(list(command))
        if "read-flash" in command:
            read_index = sum("read-flash" in call for call in self.calls) - 1
            if read_index == 0 and self.before_first_read:
                self.before_first_read()
            path = Path(command[-1])
            self.staging = path.parent
            response = self.responses.get(read_index, b"\xff" * int(command[-2], 0))
            if isinstance(response, BaseException):
                raise response
            if response is not None:
                path.write_bytes(response)
        else:
            assert "write-flash" in command
            # All writes must be one invocation after both protected reads.
            assert len(self.calls) == 3 and all("read-flash" in call for call in self.calls[:2])
            tail = command[command.index("--flash-freq") + 2:]
            assert len(tail) == 10
            for offset, filename in zip(tail[::2], tail[1::2]):
                path = Path(filename)
                assert path.parent == self.staging and path.stat().st_mode & 0o777 == 0o600
                self.written.append((int(offset, 0), path.name, path.read_bytes()))
            assert self.staging.stat().st_mode & 0o777 == 0o700
            if self.fail_write:
                raise subprocess.CalledProcessError(2, command)
        return subprocess.CompletedProcess(command, 0)


class FlashFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="flash-firmware-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bundle = self.root / "bundle"
        self.bundle.mkdir()
        self.manifest_path = self.bundle / "flash-manifest.json"
        self.provisioning_path = self.root / "provisioning-nvs.bin"
        self.write_bundle()

    def save_manifest(self):
        self.manifest_path.write_text(json.dumps(self.manifest))

    def write_bundle(self, partitions=None, flags=0):
        partitions = partitions if partitions is not None else PARTITIONS
        locations = {name: {"offset": offset, "size": size} for name, _, _, offset, size in partitions}
        self.images = {
            "bootloader.bin": (0, b"\xe9" + b"B" * 63),
            "partition-table.bin": (0x8000, make_partition_table(partitions, flags)),
            "ota_data_initial.bin": (locations["otadata"]["offset"], b"\xff" * locations["otadata"]["size"]),
            "angst-esp32s3.bin": (locations["ota_0"]["offset"], b"\xe9" + b"A" * 511),
        }
        self.manifest = {
            "schema_version": 1, "chip": "esp32s3", "flash_size": "16MB",
            "flash_mode": "dio", "flash_freq": "80m", "esptool_version": "5.4.0",
            "vendor_id": 0xFFF1, "product_id": 0x8000,
            "provisioning": locations["nvs"], "matter": locations["matter"],
            "protected_ranges": [{"name": name, **locations[name]} for name in ("nvs", "matter")],
            "app_partitions": [{"name": name, **locations[name]} for name in ("ota_0", "ota_1")],
            "segments": [],
        }
        for name, (offset, content) in self.images.items():
            (self.bundle / name).write_bytes(content)
            self.manifest["segments"].append({
                "file": name, "offset": offset, "size": len(content),
                "sha256": hashlib.sha256(content).hexdigest(),
            })
        self.provisioning_path.write_bytes(PROVISIONING)
        self.save_manifest()

    def update_image(self, name, content):
        (self.bundle / name).write_bytes(content)
        segment = next(item for item in self.manifest["segments"] if item["file"] == name)
        segment["size"], segment["sha256"] = len(content), hashlib.sha256(content).hexdigest()
        self.save_manifest()

    def install(self, runner):
        with mock.patch.object(flasher.importlib.metadata, "version", return_value="5.4.0"):
            flasher.install(self.manifest_path, self.provisioning_path, "FAKE-USB-PORT", runner=runner)

    def assert_refused_before_device(self):
        runner = FakeEsptool()
        with self.assertRaises(flasher.InstallError):
            self.install(runner)
        self.assertEqual(runner.calls, [])

    def test_public_fixture_is_official_reference(self):
        self.assertEqual(len(PROVISIONING), 0x6000)
        self.assertEqual(hashlib.sha256(PROVISIONING).hexdigest(),
                         "bdb28d0c95a44b72642fab3c69f04992b1079c7ce052cc908c98eade2cc68064")

    def test_fresh_install_reads_both_ranges_then_writes_once(self):
        runner = FakeEsptool()
        self.install(runner)
        self.assertEqual(len(runner.calls), 3)
        self.assertEqual([call[call.index("--before") + 1] for call in runner.calls],
                         ["default-reset", "no-reset", "no-reset"])
        self.assertEqual([call[call.index("--after") + 1] for call in runner.calls],
                         ["no-reset", "no-reset", "hard-reset"])
        self.assertEqual([tuple(map(lambda value: int(value, 0), call[-3:-1])) for call in runner.calls[:2]],
                         [(0x9000, 0x6000), (0xC20000, 0x20000)])
        self.assertEqual([(offset, name) for offset, name, _ in runner.written], [
            (0, "bootloader.bin"), (0x8000, "partition-table.bin"), (0x9000, "provisioning-nvs.bin"),
            (0xF000, "ota_data_initial.bin"), (0x20000, "angst-esp32s3.bin"),
        ])
        expected = {name: data for name, (_, data) in self.images.items()}
        expected["provisioning-nvs.bin"] = PROVISIONING
        self.assertEqual({name: data for _, name, data in runner.written}, expected)
        self.assertFalse(runner.staging.exists(), "private staging directory must be cleaned")

    def test_one_programmed_byte_in_either_protected_range_refuses_all_writes(self):
        for index, size in enumerate((0x6000, 0x20000)):
            for position in (0, size // 2, size - 1):
                with self.subTest(range=index, position=position):
                    existing = bytearray(b"\xff" * size)
                    existing[position] = 0
                    runner = FakeEsptool({index: existing})
                    with self.assertRaises(flasher.InstallError):
                        self.install(runner)
                    self.assertEqual(len(runner.calls), index + 1)
                    self.assertEqual(runner.written, [])
                    self.assertTrue(all("write-flash" not in call for call in runner.calls))

    def test_failed_missing_short_and_long_reads_never_write(self):
        for index, size in enumerate((0x6000, 0x20000)):
            for label, response, error in (
                ("missing", None, flasher.InstallError),
                ("short", b"\xff" * (size - 1), flasher.InstallError),
                ("long", b"\xff" * (size + 1), flasher.InstallError),
                ("failed", subprocess.CalledProcessError(2, ["fake-esptool"]), subprocess.CalledProcessError),
            ):
                with self.subTest(range=index, condition=label):
                    runner = FakeEsptool({index: response})
                    with self.assertRaises(error):
                        self.install(runner)
                    self.assertEqual(runner.written, [])
                    self.assertTrue(all("write-flash" not in call for call in runner.calls))

    def test_missing_or_changed_bundle_files_fail_before_device_access(self):
        for filename in self.images:
            for operation in ("missing", "checksum"):
                with self.subTest(file=filename, operation=operation):
                    self.write_bundle()
                    path = self.bundle / filename
                    if operation == "missing":
                        path.unlink()
                    else:
                        content = bytearray(path.read_bytes())
                        content[-1] ^= 1
                        path.write_bytes(content)
                    self.assert_refused_before_device()

    def test_manifest_settings_ranges_and_paths_fail_before_device(self):
        mutations = [
            ("schema", lambda m: m.update(schema_version=0)),
            ("boolean schema", lambda m: m.update(schema_version=True)),
            ("chip", lambda m: m.update(chip="esp32")),
            ("flash size", lambda m: m.update(flash_size="8MB")),
            ("mode", lambda m: m.update(flash_mode="qio")),
            ("frequency", lambda m: m.update(flash_freq="40m")),
            ("tool", lambda m: m.update(esptool_version="5.3.0")),
            ("vendor", lambda m: m.update(vendor_id=0)),
            ("boolean vendor", lambda m: m.update(vendor_id=True)),
            ("product", lambda m: m.update(product_id=0x10000)),
            ("incomplete", lambda m: m["segments"].pop()),
            ("duplicate", lambda m: m["segments"].__setitem__(0, copy.deepcopy(m["segments"][1]))),
            ("relative path", lambda m: m["segments"][0].update(file="../bootloader.bin")),
            ("absolute path", lambda m: m["segments"][0].update(file="/bootloader.bin")),
            ("negative offset", lambda m: m["segments"][0].update(offset=-4096)),
            ("unaligned", lambda m: m["segments"][0].update(offset=1)),
            ("boolean offset", lambda m: m["segments"][0].update(offset=False)),
            ("outside flash", lambda m: m["segments"][0].update(offset=0x1000000)),
            ("empty segment", lambda m: m["segments"][0].update(size=0)),
            ("boolean size", lambda m: m["segments"][0].update(size=True)),
            ("negative size", lambda m: m["segments"][0].update(size=-1)),
            ("missing checksum", lambda m: m["segments"][0].pop("sha256")),
            ("missing protected range", lambda m: m["protected_ranges"].pop()),
            ("swapped protected ranges", lambda m: m["protected_ranges"].reverse()),
            ("altered NVS", lambda m: m["provisioning"].update(offset=0x10000)),
            ("altered Matter", lambda m: m["matter"].update(offset=0xE00000)),
            ("altered apps", lambda m: m["app_partitions"][0].update(size=0x1000)),
        ]
        for label, change in mutations:
            with self.subTest(condition=label):
                self.write_bundle()
                change(self.manifest)
                self.save_manifest()
                self.assert_refused_before_device()

    def test_self_consistent_malicious_partition_layouts_are_refused(self):
        for target, field, replacement in (
            ("nvs", 3, 0x12000), ("matter", 3, 0xE00000), ("matter", 4, 0x10000),
            ("ota_0", 4, 0x500000), ("ota_1", 4, 0x500000),
            ("phy_init", 3, 0x12000), ("otadata", 4, 0x1000),
        ):
            with self.subTest(partition=target, field=field):
                partitions = [list(row) for row in PARTITIONS]
                next(row for row in partitions if row[0] == target)[field] = replacement
                self.write_bundle(partitions)
                self.assert_refused_before_device()
        # This would read a fake blank Matter range and place an application on
        # the real board's Matter NVS if layout were trusted only from manifest.
        partitions = [list(row) for row in PARTITIONS]
        next(row for row in partitions if row[0] == "matter")[3:] = [0xE00000, 0x20000]
        next(row for row in partitions if row[0] == "ota_0")[3:] = [0xC20000, 0x20000]
        self.write_bundle(partitions)
        self.assert_refused_before_device()
        self.write_bundle([row for row in PARTITIONS if row[0] != "phy_init"])
        self.assert_refused_before_device()
        self.write_bundle(PARTITIONS + [("extra", 1, 2, 0xE00000, 0x1000)])
        self.assert_refused_before_device()

    def test_partition_md5_flags_and_unexpected_data_are_checked(self):
        self.write_bundle(flags=1)
        self.assert_refused_before_device()
        for label, change in (
            ("bad MD5", lambda data: data.__setitem__(6 * 32 + 16, data[6 * 32 + 16] ^ 1)),
            ("missing MD5", lambda data: data.__setitem__(slice(6 * 32, 7 * 32), b"\xff" * 32)),
            ("after MD5", lambda data: data.__setitem__(7 * 32, 0)),
        ):
            with self.subTest(condition=label):
                self.write_bundle()
                table = bytearray((self.bundle / "partition-table.bin").read_bytes())
                change(table)
                self.update_image("partition-table.bin", bytes(table))
                self.assert_refused_before_device()

    def test_corrupt_provisioning_and_wrong_attestation_ids_fail_before_device(self):
        for label, data in (
            ("short", PROVISIONING[:-1]), ("long", PROVISIONING + b"\xff"),
            ("erased", b"\xff" * len(PROVISIONING)),
            ("bad page CRC", PROVISIONING[:28] + bytes((PROVISIONING[28] ^ 1,)) + PROVISIONING[29:]),
            ("bad bitmap", PROVISIONING[:32] + b"\xff" + PROVISIONING[33:]),
            ("bad entry CRC", PROVISIONING[:68] + bytes((PROVISIONING[68] ^ 1,)) + PROVISIONING[69:]),
            ("bad blob CRC", PROVISIONING[:128] + bytes((PROVISIONING[128] ^ 1,)) + PROVISIONING[129:]),
            ("unexpected trailing data", PROVISIONING[:384] + b"\0" + PROVISIONING[385:]),
            ("vendor mismatch", record_mutation(lambda r: struct.pack_into("<H", r, 14, 0xFFF2))),
            ("product mismatch", record_mutation(lambda r: struct.pack_into("<H", r, 16, 0x8001))),
            ("record schema", record_mutation(lambda r: struct.pack_into("<H", r, 4, 2))),
            ("record magic", record_mutation(lambda r: r.__setitem__(0, 0))),
            ("discriminator", record_mutation(lambda r: struct.pack_into("<H", r, 8, 4096))),
            ("iterations", record_mutation(lambda r: struct.pack_into("<I", r, 10, 999))),
            ("empty serial", record_mutation(lambda r: r.__setitem__(18, 0))),
            ("long serial", record_mutation(lambda r: r.__setitem__(18, 33))),
            ("serial padding", record_mutation(lambda r: r.__setitem__(50, 1))),
            ("invalid UTF8", record_mutation(lambda r: r.__setitem__(19, 0xFF))),
            ("control serial", record_mutation(lambda r: r.__setitem__(19, 0))),
        ):
            with self.subTest(condition=label):
                self.provisioning_path.write_bytes(data)
                self.assert_refused_before_device()

    def test_missing_provisioning_esptool_and_wrong_esptool_version_fail_before_device(self):
        self.provisioning_path.unlink()
        self.assert_refused_before_device()
        self.provisioning_path.write_bytes(PROVISIONING)
        for missing in (False, True):
            with self.subTest(missing=missing):
                runner = FakeEsptool()
                kwargs = ({"side_effect": flasher.importlib.metadata.PackageNotFoundError("esptool")}
                          if missing else {"return_value": "5.3.0"})
                with mock.patch.object(flasher.importlib.metadata, "version", **kwargs):
                    with self.assertRaises(flasher.InstallError):
                        flasher.install(self.manifest_path, self.provisioning_path, "FAKE-USB-PORT", runner=runner)
                self.assertEqual(runner.calls, [])

    def test_verified_inputs_are_staged_before_device_access(self):
        originals = {name: content for name, (_, content) in self.images.items()}

        def replace_original_inputs():
            for name in originals:
                (self.bundle / name).write_bytes(b"changed after verification")
            self.provisioning_path.write_bytes(b"changed after verification")
            self.manifest_path.write_text("{}")

        runner = FakeEsptool(before_first_read=replace_original_inputs)
        self.install(runner)
        originals["provisioning-nvs.bin"] = PROVISIONING
        self.assertEqual({name: content for _, name, content in runner.written}, originals)
        self.assertFalse(runner.staging.exists())

    def test_failed_write_is_not_retried_and_private_staging_is_removed(self):
        runner = FakeEsptool(fail_write=True)
        with self.assertRaises(subprocess.CalledProcessError):
            self.install(runner)
        self.assertEqual(sum("write-flash" in call for call in runner.calls), 1)
        self.assertFalse(runner.staging.exists())


if __name__ == "__main__":
    unittest.main()
