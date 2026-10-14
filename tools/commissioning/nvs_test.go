package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"strings"
	"testing"
)

func nvsCompatibilityIdentity() identity {
	device := identity{
		Discriminator: 1234, SPAKE2PIterations: 10000,
		VendorID: 0xfff1, ProductID: 0x8000, SerialNumber: "NVS-COMPAT-0001",
		SPAKE2PSalt: make([]byte, 32), SPAKE2PVerifier: make([]byte, 97), RotatingID: make([]byte, 16),
	}
	for i := range device.SPAKE2PSalt {
		device.SPAKE2PSalt[i] = byte(i)
	}
	for i := range device.SPAKE2PVerifier {
		device.SPAKE2PVerifier[i] = byte(32 + i)
	}
	for i := range device.RotatingID {
		device.RotatingID[i] = byte(129 + i)
	}
	return device
}

func TestProvisioningNVSMatchesOfficialGenerator(t *testing.T) {
	image, err := makeProvisioningNVS(nvsCompatibilityIdentity())
	if err != nil {
		t.Fatal(err)
	}
	// Independently encoded the AMPI fixture with Python struct/zlib, then ran
	// pinned ESP-IDF 6.1 esp_idf_nvs_partition_gen.nvs_open(..., 0x5000, V2),
	// write_namespace("matter_prov"), write_entry("identity", hex, "hex2bin").
	// The resulting complete 0x6000-byte image has this SHA-256. Keeping the
	// reference digest makes ordinary Go tests independent of Python and ESP-IDF.
	const referenceSHA256 = "bdb28d0c95a44b72642fab3c69f04992b1079c7ce052cc908c98eade2cc68064"
	digest := sha256.Sum256(image)
	if len(image) != 0x6000 || hex.EncodeToString(digest[:]) != referenceSHA256 {
		t.Fatal("provisioning NVS differs from the independently generated official ESP-IDF fixture")
	}
	second, err := makeProvisioningNVS(nvsCompatibilityIdentity())
	if err != nil || !bytes.Equal(second, image) {
		t.Fatal("provisioning NVS is not deterministic")
	}
	if !bytes.Equal(image[4096:], bytes.Repeat([]byte{0xff}, 5*4096)) {
		t.Fatal("unused NVS pages must remain erased")
	}
	if !bytes.Equal(image[32:35], []byte{0xaa, 0xaa, 0xfa}) {
		t.Fatal("NVS entry bitmap must mark exactly ten entries written")
	}
	if !bytes.Equal(image[35:64], bytes.Repeat([]byte{0xff}, 29)) {
		t.Fatal("unused NVS bitmap entries must remain empty")
	}
}

func TestProvisioningRecordPortableLayout(t *testing.T) {
	device := nvsCompatibilityIdentity()
	record, err := makeProvisioningRecord(device)
	if err != nil {
		t.Fatal(err)
	}
	// Python struct/zlib reference for the same public, synthetic fixture.
	const referenceSHA256 = "387341da285941d5c209fbafe8da291415a42f7c10eaca4a00c2119c4578a110"
	digest := sha256.Sum256(record)
	if len(record) != 200 || hex.EncodeToString(digest[:]) != referenceSHA256 {
		t.Fatal("AMPI schema does not match the independent portable record fixture")
	}
	if string(record[:4]) != "AMPI" || binary.LittleEndian.Uint16(record[4:6]) != 1 ||
		binary.LittleEndian.Uint16(record[6:8]) != 200 ||
		binary.LittleEndian.Uint16(record[8:10]) != device.Discriminator ||
		binary.LittleEndian.Uint32(record[10:14]) != uint32(device.SPAKE2PIterations) ||
		binary.LittleEndian.Uint16(record[14:16]) != device.VendorID ||
		binary.LittleEndian.Uint16(record[16:18]) != device.ProductID ||
		int(record[18]) != len(device.SerialNumber) ||
		string(record[19:19+len(device.SerialNumber)]) != device.SerialNumber ||
		!bytes.Equal(record[51:83], device.SPAKE2PSalt) ||
		!bytes.Equal(record[83:180], device.SPAKE2PVerifier) ||
		!bytes.Equal(record[180:196], device.RotatingID) ||
		binary.LittleEndian.Uint32(record[196:200]) != 0x15d1a5cd {
		t.Fatal("AMPI fields are not encoded in the firmware's portable layout")
	}
	if !bytes.Equal(record[19+len(device.SerialNumber):51], make([]byte, 32-len(device.SerialNumber))) {
		t.Fatal("unused serial bytes must be zero-filled")
	}
	device.Passcode = 87654321
	device.SoftwareVersion = 42
	device.SoftwareVersionString = "42.0"
	device.VendorName = "New manufacturer label"
	device.ProductName = "New product label"
	device.NodeLabel = "New room label"
	device.HardwareVersion = 17
	device.HardwareVersionString = "Revision label"
	metadataOnly, err := makeProvisioningRecord(device)
	if err != nil || !bytes.Equal(record, metadataOnly) {
		t.Fatal("record must exclude the setup passcode and descriptive/version metadata")
	}
}

func TestProvisioningRecordRejectsInvalidInputs(t *testing.T) {
	for _, test := range []struct {
		name   string
		change func(*identity)
	}{
		{"discriminator", func(d *identity) { d.Discriminator = 4096 }},
		{"too few iterations", func(d *identity) { d.SPAKE2PIterations = 999 }},
		{"too many iterations", func(d *identity) { d.SPAKE2PIterations = 100001 }},
		{"empty serial", func(d *identity) { d.SerialNumber = "" }},
		{"long serial", func(d *identity) { d.SerialNumber = strings.Repeat("A", 33) }},
		{"long UTF-8 serial", func(d *identity) { d.SerialNumber = strings.Repeat("é", 17) }},
		{"invalid UTF-8 serial", func(d *identity) { d.SerialNumber = "bad\xff" }},
		{"NUL serial", func(d *identity) { d.SerialNumber = "bad\x00" }},
		{"control serial", func(d *identity) { d.SerialNumber = "bad\u0085" }},
		{"short salt", func(d *identity) { d.SPAKE2PSalt = d.SPAKE2PSalt[:31] }},
		{"long salt", func(d *identity) { d.SPAKE2PSalt = append(d.SPAKE2PSalt, 1) }},
		{"short verifier", func(d *identity) { d.SPAKE2PVerifier = d.SPAKE2PVerifier[:96] }},
		{"long verifier", func(d *identity) { d.SPAKE2PVerifier = append(d.SPAKE2PVerifier, 1) }},
		{"short rotating ID", func(d *identity) { d.RotatingID = d.RotatingID[:15] }},
		{"long rotating ID", func(d *identity) { d.RotatingID = append(d.RotatingID, 1) }},
	} {
		t.Run(test.name, func(t *testing.T) {
			device := nvsCompatibilityIdentity()
			test.change(&device)
			if image, err := makeProvisioningNVS(device); err == nil || image != nil {
				t.Fatal("invalid identity produced a provisioning image")
			}
		})
	}
	for _, serial := range []string{strings.Repeat("A", 32), strings.Repeat("é", 16)} {
		device := nvsCompatibilityIdentity()
		device.SerialNumber = serial
		if _, err := makeProvisioningNVS(device); err != nil {
			t.Fatal("valid maximum-length serial was rejected:", err)
		}
	}
}
