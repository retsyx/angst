#!/usr/bin/env python3
"""Check USB bundle metadata, release contents, and failure handling without hardware."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("package_firmware", ROOT / "tools" / "package_firmware.py")
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)
PUBLIC_FILES = {
    "bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "angst-esp32s3.bin",
    "flash-manifest.json", "flash_firmware.py", "README.txt",
}


def table_bytes(partitions):
    data = b"".join(struct.pack("<HBBII16sI", 0x50aa, kind, subtype, offset, size,
                                name.encode(), flags)
                    for name, kind, subtype, offset, size, flags in partitions)
    data += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(data).digest()
    return data.ljust(0xc00, b"\xff")


def verify_archive(path):
    with zipfile.ZipFile(path) as archive:
        expected = {f"angst-esp32s3-flash/{name}" for name in PUBLIC_FILES}
        if set(archive.namelist()) != expected or len(archive.namelist()) != len(expected):
            raise ValueError("Flash bundle contains missing, duplicate, or nonpublic files")
        prefix = "angst-esp32s3-flash/"
        manifest = json.loads(archive.read(prefix + "flash-manifest.json"))
        if manifest["schema_version"] != 1 or manifest["chip"] != "esp32s3":
            raise ValueError("Flash bundle has an unsupported schema or chip")
        if manifest["esptool_version"] != "5.4.0":
            raise ValueError("Flash bundle has an unsupported esptool version")
        if manifest["provisioning"] != {"offset": 0x9000, "size": 0x6000}:
            raise ValueError("Flash bundle provisioning range differs from the commissioner")
        segment_names = {segment["file"] for segment in manifest["segments"]}
        if segment_names != PUBLIC_FILES - {"flash-manifest.json", "flash_firmware.py", "README.txt"}:
            raise ValueError("Flash bundle has missing or nonpublic firmware segments")
        for segment in manifest["segments"]:
            data = archive.read(prefix + segment["file"])
            if len(data) != segment["size"] or hashlib.sha256(data).hexdigest() != segment["sha256"]:
                raise ValueError("Flash bundle segment size or checksum does not match the manifest")
        for member in archive.infolist():
            if member.filename.startswith("/") or ".." in Path(member.filename).parts:
                raise ValueError("Flash bundle has an unsafe extraction path")
        return manifest


class FirmwareBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="firmware-bundle-test-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.build = self.base / "build"
        (self.build / "bootloader").mkdir(parents=True)
        (self.build / "partition_table").mkdir()
        (self.build / "config").mkdir()
        self.output = self.base / "dist" / "angst-esp32s3-flash.zip"
        self.helper = self.base / "flash_firmware.py"
        self.helper.write_text("#!/usr/bin/env python3\n# Public flash helper fixture.\n")
        self.readme = self.base / "FLASHING.md"
        self.readme.write_text("Fresh boards require a unique local provisioning image.\n")
        self.files = {
            "bootloader/bootloader.bin": b"\xe9" + b"boot" * 2048,
            "partition_table/partition-table.bin": b"",
            "ota_data_initial.bin": b"\xff" * 0x2000,
            "native-display.bin": b"\xe9" + b"app" * 4096,
        }
        self.args = {
            "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
            "extra_esptool_args": {"chip": "esp32s3"},
            "flash_files": {
                "0x0": "bootloader/bootloader.bin",
                "0x8000": "partition_table/partition-table.bin",
                "0xf000": "ota_data_initial.bin",
                "0x20000": "native-display.bin",
            },
        }
        for role, offset, filename in (
            ("bootloader", "0x0", "bootloader/bootloader.bin"),
            ("partition-table", "0x8000", "partition_table/partition-table.bin"),
            ("otadata", "0xf000", "ota_data_initial.bin"),
            ("app", "0x20000", "native-display.bin"),
        ):
            self.args[role] = {"offset": offset, "file": filename, "encrypted": "false"}
        self.config = {
            "IDF_TARGET": "esp32s3", "ESPTOOLPY_FLASHMODE": "dio", "ESPTOOLPY_FLASHSIZE": "16MB",
            "ESPTOOLPY_FLASHFREQ": "80m", "PARTITION_TABLE_OFFSET": 0x8000,
            "PARTITION_TABLE_MD5": True, "ESP_MATTER_NVS_PART_NAME": "matter",
            "DEVICE_VENDOR_ID": 0xfff1, "DEVICE_PRODUCT_ID": 0x8000,
        }
        self.partitions = [
            ("nvs", 1, 2, 0x9000, 0x6000, 0),
            ("otadata", 1, 0, 0xf000, 0x2000, 0),
            ("phy_init", 1, 1, 0x11000, 0x1000, 0),
            ("ota_0", 0, 0x10, 0x20000, 0x600000, 0),
            ("ota_1", 0, 0x11, 0x620000, 0x600000, 0),
            ("matter", 1, 2, 0xc20000, 0x20000, 0),
        ]

    def write_metadata(self):
        self.files["partition_table/partition-table.bin"] = table_bytes(self.partitions)
        for name, data in self.files.items():
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.build / "flasher_args.json").write_text(json.dumps(self.args))
        (self.build / "config" / "sdkconfig.json").write_text(json.dumps(self.config))

    def package(self):
        self.write_metadata()
        return PACKAGER.package_firmware(self.build, self.output, self.helper, self.readme)

    def assert_rejected(self, message=None):
        self.write_metadata()
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_bytes(b"preserve-existing-artifact")
        with self.assertRaises((ValueError, KeyError), msg=message):
            PACKAGER.package_firmware(self.build, self.output, self.helper, self.readme)
        self.assertEqual(self.output.read_bytes(), b"preserve-existing-artifact")

    def test_bundle_contains_only_public_outputs_and_verifiable_manifest(self):
        (self.build / "generated").mkdir()
        for name in ("identity.json", "onboarding.txt", "qrcode.png", "provisioning-nvs.bin"):
            (self.build / "generated" / name).write_bytes(b"PRIVATE-DO-NOT-PUBLISH")
        self.args["flash_settings"]["private_unused_setting"] = "PRIVATE-DO-NOT-PUBLISH"
        self.config["PRIVATE_UNUSED_CONFIG"] = "PRIVATE-DO-NOT-PUBLISH"
        manifest = self.package()
        self.assertEqual(verify_archive(self.output), manifest)
        self.assertEqual(manifest["matter"], {"offset": 0xc20000, "size": 0x20000})
        self.assertEqual(manifest["vendor_id"], 0xfff1)
        self.assertEqual(manifest["product_id"], 0x8000)
        self.assertEqual(manifest["app_partitions"], [
            {"name": "ota_0", "offset": 0x20000, "size": 0x600000},
            {"name": "ota_1", "offset": 0x620000, "size": 0x600000},
        ])
        self.assertEqual(manifest["protected_ranges"], [
            {"name": "nvs", "offset": 0x9000, "size": 0x6000},
            {"name": "matter", "offset": 0xc20000, "size": 0x20000},
        ])
        with zipfile.ZipFile(self.output) as archive:
            for member in archive.namelist():
                self.assertNotIn(b"PRIVATE-DO-NOT-PUBLISH", archive.read(member))
            self.assertEqual(archive.read("angst-esp32s3-flash/flash_firmware.py"), self.helper.read_bytes())
            self.assertEqual(archive.read("angst-esp32s3-flash/README.txt"), self.readme.read_bytes())

    def test_bundle_is_deterministic(self):
        self.package()
        first = self.output.read_bytes()
        self.package()
        self.assertEqual(self.output.read_bytes(), first)

    def test_self_consistent_unsupported_flash_mode_is_rejected(self):
        self.args["flash_settings"]["flash_mode"] = self.config["ESPTOOLPY_FLASHMODE"] = "qio"
        self.assert_rejected()

    def test_self_consistent_unsupported_flash_frequency_is_rejected(self):
        self.args["flash_settings"]["flash_freq"] = self.config["ESPTOOLPY_FLASHFREQ"] = "40m"
        self.assert_rejected()

    def test_self_consistent_matter_relocation_is_rejected(self):
        self.config["ESP_MATTER_NVS_PART_NAME"] = "matter_alt"
        self.partitions[-1] = ("matter_alt", 1, 2, 0xc40000, 0x30000, 0)
        self.assert_rejected()

    def test_self_consistent_other_application_slot_is_rejected(self):
        self.args["app"]["offset"] = "0x620000"
        self.args["flash_files"]["0x620000"] = self.args["flash_files"].pop("0x20000")
        self.assert_rejected()

    def test_additional_partition_is_rejected(self):
        self.partitions.append(("extra", 1, 2, 0xc40000, 0x1000, 0))
        self.assert_rejected()

    def test_application_partition_flags_are_rejected(self):
        self.partitions[3] = ("ota_0", 0, 0x10, 0x20000, 0x600000, 1)
        self.assert_rejected()

    def test_unexpected_flash_file_cannot_publish_private_nvs(self):
        self.args["flash_files"]["0x9000"] = "private-nvs.bin"
        self.files["private-nvs.bin"] = b"PRIVATE-DO-NOT-PUBLISH"
        self.assert_rejected()

    def test_build_output_cannot_escape_build_directory(self):
        outside = self.base / "outside.bin"
        outside.write_bytes(b"\xe9private")
        self.args["app"]["file"] = "../outside.bin"
        self.args["flash_files"]["0x20000"] = "../outside.bin"
        self.assert_rejected()

    def test_conflicting_sdk_settings_are_rejected(self):
        self.config["ESPTOOLPY_FLASHMODE"] = "qio"
        self.assert_rejected()

    def test_wrong_target_is_rejected(self):
        self.args["extra_esptool_args"]["chip"] = "esp32"
        self.assert_rejected()

    def test_encrypted_segment_is_rejected(self):
        self.args["app"]["encrypted"] = "true"
        self.assert_rejected()

    def test_partition_checksum_is_verified(self):
        self.write_metadata()
        path = self.build / "partition_table" / "partition-table.bin"
        data = bytearray(path.read_bytes())
        data[12] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "checksum"):
            PACKAGER.package_firmware(self.build, self.output, self.helper, self.readme)
        self.assertFalse(self.output.exists())

    def test_missing_partition_checksum_is_rejected(self):
        self.write_metadata()
        path = self.build / "partition_table" / "partition-table.bin"
        data = bytearray(path.read_bytes())
        data[len(self.partitions) * 32:] = b"\xff" * (len(data) - len(self.partitions) * 32)
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "checksum"):
            PACKAGER.package_firmware(self.build, self.output, self.helper, self.readme)

    def test_provisioning_range_cannot_drift_from_go_writer(self):
        self.partitions[0] = ("nvs", 1, 2, 0x9000, 0x5000, 0)
        self.assert_rejected()

    def test_partition_overlap_is_rejected(self):
        self.partitions[-1] = ("matter", 1, 2, 0xc10000, 0x20000, 0)
        self.assert_rejected()

    def test_application_must_fit_its_partition(self):
        self.partitions[3] = ("ota_0", 0, 0x10, 0x20000, 0x1000, 0)
        self.assert_rejected()

    def test_partition_must_fit_configured_flash(self):
        self.args["flash_settings"]["flash_size"] = self.config["ESPTOOLPY_FLASHSIZE"] = "8MB"
        self.assert_rejected()

    def test_initial_ota_data_cannot_include_existing_boot_state(self):
        self.files["ota_data_initial.bin"] = b"\x01" + b"\xff" * (0x2000 - 1)
        self.assert_rejected()

    def test_duplicate_flash_offsets_are_rejected(self):
        self.args["flash_files"]["32768"] = "partition_table/partition-table.bin"
        self.assert_rejected()

    def test_missing_helper_does_not_replace_existing_output(self):
        self.helper.unlink()
        self.write_metadata()
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_bytes(b"preserve-existing-artifact")
        with self.assertRaises(OSError):
            PACKAGER.package_firmware(self.build, self.output, self.helper, self.readme)
        self.assertEqual(self.output.read_bytes(), b"preserve-existing-artifact")

    def test_archive_audit_rejects_added_private_file(self):
        self.package()
        with zipfile.ZipFile(self.output, "a") as archive:
            archive.writestr("angst-esp32s3-flash/identity.json", b"private")
        with self.assertRaisesRegex(ValueError, "nonpublic"):
            verify_archive(self.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, help="Audit a real packaged release instead of running unit tests")
    arguments = parser.parse_args()
    if arguments.bundle:
        verify_archive(arguments.bundle)
        # Exercise the real producer/consumer interface as well as the archive
        # allowlist. This opens no serial port and does not require esptool.
        helper_path = ROOT / "tools/flash_firmware.py"
        with tempfile.TemporaryDirectory(prefix="firmware-bundle-audit-") as directory:
            with zipfile.ZipFile(arguments.bundle) as archive:
                if archive.read("angst-esp32s3-flash/flash_firmware.py") != helper_path.read_bytes():
                    raise ValueError("Bundled flash helper differs from the tested source")
                archive.extractall(directory)
            spec = importlib.util.spec_from_file_location("audited_flash_helper", helper_path)
            helper = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(helper)
            helper.load_bundle(Path(directory) / "angst-esp32s3-flash/flash-manifest.json")
        print("Firmware release bundle audit passed: public files, checksums, and flash helper compatibility.")
    else:
        result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(FirmwareBundleTests))
        raise SystemExit(not result.wasSuccessful())


if __name__ == "__main__":
    main()
