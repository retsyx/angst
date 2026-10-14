package main

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"math/big"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/boombuler/barcode/qr"
)

type identity struct {
	SchemaVersion         int    `json:"schema_version"`
	VendorName            string `json:"vendor_name"`
	ProductName           string `json:"product_name"`
	NodeLabel             string `json:"node_label"`
	VendorID              uint16 `json:"vendor_id"`
	ProductID             uint16 `json:"product_id"`
	SoftwareVersion       uint32 `json:"software_version"`
	SoftwareVersionString string `json:"software_version_string"`
	HardwareVersion       uint16 `json:"hardware_version"`
	HardwareVersionString string `json:"hardware_version_string"`
	SerialNumber          string `json:"serial_number"`
	Passcode              uint32 `json:"passcode"`
	Discriminator         uint16 `json:"discriminator"`
	SPAKE2PIterations     int    `json:"spake2p_iterations"`
	SPAKE2PSalt           []byte `json:"spake2p_salt"`
	SPAKE2PVerifier       []byte `json:"spake2p_verifier"`
	RotatingID            []byte `json:"rotating_id"`
	ManualCode            string `json:"manual_code"`
	QRPayload             string `json:"qr_payload"`
}

func main() {
	if err := run(os.Args[1:], os.Stdout, os.Stderr); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return
		}
		fmt.Fprintln(os.Stderr, "commissioning:", err)
		os.Exit(1)
	}
}

func run(args []string, stdout, stderr io.Writer) error {
	fs := flag.NewFlagSet("commissioning", flag.ContinueOnError)
	fs.SetOutput(stderr)
	out := fs.String("out", "generated/matter", "output directory (paths are relative to the working directory)")
	regenerate := fs.Bool("regenerate", false, "replace the device identity and pairing credentials; requires re-pairing")
	vendorName := fs.String("vendor-name", "Angst", "Matter manufacturer name (maximum 32 UTF-8 bytes)")
	productName := fs.String("product-name", "Angst Display", "Matter product name (maximum 32 UTF-8 bytes)")
	nodeLabel := fs.String("node-label", "Angst", "initial Matter device name (maximum 32 UTF-8 bytes)")
	vendorID := fs.Uint("vendor-id", 0xfff1, "Matter vendor ID; must match the device attestation credentials")
	productID := fs.Uint("product-id", 0x8000, "Matter product ID; must match the device attestation credentials")
	softwareVersion := fs.Uint64("software-version", 2, "numeric firmware version (1 through 4294967295)")
	softwareVersionString := fs.String("software-version-string", "2.0.0", "firmware version string (1 to 31 build-safe characters)")
	hardwareVersion := fs.Uint("hardware-version", 1, "hardware revision (0 through 65535)")
	hardwareVersionString := fs.String("hardware-version-string", "T-Display-S3 AMOLED", "hardware revision name (maximum 64 UTF-8 bytes)")
	serialNumber := fs.String("serial-number", "", "optional device serial number (maximum 32 UTF-8 bytes; random by default)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() != 0 {
		return fmt.Errorf("unexpected positional argument %q", fs.Arg(0))
	}
	if *vendorID == 0 || *vendorID > 0xfff4 || *productID == 0 || *productID > 0xffff {
		return fmt.Errorf("vendor ID must be 1 through 0xfff4 and product ID must be 1 through 0xffff")
	}
	if *softwareVersion == 0 || *softwareVersion > 0xffffffff || *hardwareVersion > 0xffff {
		return fmt.Errorf("software or hardware version is outside its supported range")
	}
	metadata := identity{
		SchemaVersion: 1, VendorName: *vendorName, ProductName: *productName, NodeLabel: *nodeLabel,
		VendorID: uint16(*vendorID), ProductID: uint16(*productID),
		SoftwareVersion: uint32(*softwareVersion), SoftwareVersionString: *softwareVersionString,
		HardwareVersion: uint16(*hardwareVersion), HardwareVersionString: *hardwareVersionString,
		SerialNumber: *serialNumber,
	}
	if err := validateMetadata(metadata, *serialNumber == ""); err != nil {
		return err
	}
	if err := os.MkdirAll(*out, 0700); err != nil {
		return err
	}
	lock := filepath.Join(*out, ".commissioning.lock")
	if err := os.Mkdir(lock, 0700); err != nil {
		return fmt.Errorf("lock output directory: %w (another generator may be running; remove a stale .commissioning.lock only after it has stopped)", err)
	}
	defer os.Remove(lock)
	path := filepath.Join(*out, "identity.json")
	var device identity
	data, err := os.ReadFile(path)
	reused := false
	if err == nil && !*regenerate {
		decoder := json.NewDecoder(bytes.NewReader(data))
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&device); err != nil {
			return fmt.Errorf("read existing identity: %w; use --regenerate only to replace its credentials", err)
		}
		if err := decoder.Decode(new(any)); err != io.EOF {
			return fmt.Errorf("existing identity must contain exactly one JSON object")
		}
		if err := validateIdentity(device); err != nil {
			return fmt.Errorf("existing identity: %w; use --regenerate only to replace its credentials", err)
		}
		reused = true
		// Existing credentials and defaults survive ordinary rebuilds. Only flags
		// explicitly provided by the caller update descriptive/version metadata.
		fs.Visit(func(f *flag.Flag) {
			switch f.Name {
			case "vendor-name":
				device.VendorName = metadata.VendorName
			case "product-name":
				device.ProductName = metadata.ProductName
			case "node-label":
				device.NodeLabel = metadata.NodeLabel
			case "vendor-id":
				device.VendorID = metadata.VendorID
			case "product-id":
				device.ProductID = metadata.ProductID
			case "software-version":
				device.SoftwareVersion = metadata.SoftwareVersion
			case "software-version-string":
				device.SoftwareVersionString = metadata.SoftwareVersionString
			case "hardware-version":
				device.HardwareVersion = metadata.HardwareVersion
			case "hardware-version-string":
				device.HardwareVersionString = metadata.HardwareVersionString
			case "serial-number":
				device.SerialNumber = metadata.SerialNumber
			}
		})
	} else {
		if err != nil && !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("read existing identity: %w", err)
		}
		device, err = newIdentity(metadata)
		if err != nil {
			return err
		}
	}
	if err := validateIdentity(device); err != nil {
		return err
	}
	device.ManualCode = manualCode(device.Discriminator, device.Passcode)
	device.QRPayload = qrPayload(device.VendorID, device.ProductID, device.Discriminator, device.Passcode, 2) // BLE
	if err := writeArtifacts(*out, device); err != nil {
		return err
	}
	action := "Generated"
	if reused {
		action = "Reused"
	}
	fmt.Fprintf(stdout, "%s Matter identity for %s (%s).\nManual pairing code: %s\nQR code: %s\n", action, device.ProductName, device.SerialNumber, device.ManualCode, filepath.Join(*out, "qrcode.png"))
	return nil
}

func randomBytes(count int) ([]byte, error) {
	data := make([]byte, count)
	_, err := rand.Read(data)
	return data, err
}

func newIdentity(device identity) (identity, error) {
	for {
		passcode, err := rand.Int(rand.Reader, big.NewInt(maxPasscode))
		if err != nil {
			return device, err
		}
		device.Passcode = uint32(passcode.Uint64()) + 1
		if validPasscode(device.Passcode) && device.Passcode != 20202021 {
			break
		}
	}
	for {
		discriminator, err := rand.Int(rand.Reader, big.NewInt(4096))
		if err != nil {
			return device, err
		}
		device.Discriminator = uint16(discriminator.Uint64())
		if device.Discriminator != 3840 {
			break
		}
	}
	var err error
	if device.SerialNumber == "" {
		serial, err := randomBytes(10)
		if err != nil {
			return device, err
		}
		device.SerialNumber = "ANGST-" + strings.ToUpper(hex.EncodeToString(serial))
	}
	device.SPAKE2PIterations = 10000
	device.SPAKE2PSalt, err = randomBytes(32)
	if err != nil {
		return device, err
	}
	device.SPAKE2PVerifier, err = spake2pVerifier(device.Passcode, device.SPAKE2PSalt, device.SPAKE2PIterations)
	if err != nil {
		return device, err
	}
	device.RotatingID, err = randomBytes(16)
	return device, err
}

func validateMetadata(device identity, allowEmptySerial bool) error {
	fields := []struct {
		name, value string
		limit       int
	}{
		{"vendor name", device.VendorName, 32}, {"product name", device.ProductName, 32},
		{"node label", device.NodeLabel, 32}, {"hardware version string", device.HardwareVersionString, 64},
		{"serial number", device.SerialNumber, 32},
	}
	for _, field := range fields {
		if field.name == "serial number" && allowEmptySerial && field.value == "" {
			continue
		}
		if len(field.value) == 0 || len(field.value) > field.limit || !utf8.ValidString(field.value) || strings.IndexFunc(field.value, unicode.IsControl) != -1 {
			return fmt.Errorf("%s must contain 1 to %d UTF-8 bytes without control characters", field.name, field.limit)
		}
	}
	// PROJECT_VER is embedded in ESP-IDF's 32-byte app description (with NUL).
	if !regexp.MustCompile(`^[A-Za-z0-9._+-]{1,31}$`).MatchString(device.SoftwareVersionString) {
		return fmt.Errorf("software version string must contain 1 to 31 letters, digits, dots, underscores, pluses, or hyphens")
	}
	if device.VendorID == 0 || device.VendorID > 0xfff4 || device.ProductID == 0 || device.SoftwareVersion == 0 {
		return fmt.Errorf("invalid vendor ID, product ID, or software version")
	}
	return nil
}

func validateIdentity(device identity) error {
	if device.SchemaVersion != 1 {
		return fmt.Errorf("unsupported identity schema version %d", device.SchemaVersion)
	}
	if err := validateMetadata(device, false); err != nil {
		return err
	}
	if !validPasscode(device.Passcode) || device.Passcode == 20202021 || device.Discriminator > 4095 || device.Discriminator == 3840 {
		return fmt.Errorf("invalid or development-default commissioning credentials")
	}
	if len(device.SPAKE2PSalt) != 32 || len(device.RotatingID) != 16 {
		return fmt.Errorf("salt must contain 32 bytes and rotating device ID must contain 16 bytes")
	}
	verifier, err := spake2pVerifier(device.Passcode, device.SPAKE2PSalt, device.SPAKE2PIterations)
	if err != nil {
		return err
	}
	if !bytes.Equal(verifier, device.SPAKE2PVerifier) {
		return fmt.Errorf("SPAKE2+ verifier does not match the stored passcode and salt")
	}
	return nil
}

func writeArtifacts(out string, device identity) error {
	identityJSON, err := json.MarshalIndent(device, "", "  ")
	if err != nil {
		return err
	}
	qrPNG, err := makeQRCode(device.QRPayload)
	if err != nil {
		return err
	}
	provisioningNVS, err := makeProvisioningNVS(device)
	if err != nil {
		return err
	}
	onboarding := fmt.Sprintf("%s\nManufacturer: %s\nSerial number: %s\nFirmware: %s (%d)\n\nManual pairing code: %s\nQR payload: %s\n\nScan qrcode.png using your Matter controller, then select the Wi-Fi network.\nKeep identity.json private and reuse it for this device's subsequent firmware builds.\n", device.ProductName, device.VendorName, device.SerialNumber, device.SoftwareVersionString, device.SoftwareVersion, device.ManualCode, device.QRPayload)
	artifacts := []struct {
		name string
		data []byte
	}{
		// Write the identity first so interrupted generation never loses the
		// credentials already chosen for this device.
		{"identity.json", append(identityJSON, '\n')},
		{"provisioning-nvs.bin", provisioningNVS},
		{"matter_identity.h", makeHeader(device)},
		{"matter_identity.cmake", []byte(fmt.Sprintf("# Generated by tools/commissioning.\nset(PROJECT_VER \"%s\")\nset(PROJECT_VER_NUMBER %d)\n", device.SoftwareVersionString, device.SoftwareVersion))},
		{"onboarding.txt", []byte(onboarding)},
		{"qrcode.png", qrPNG},
	}
	for _, artifact := range artifacts {
		if err := atomicWrite(filepath.Join(out, artifact.name), artifact.data); err != nil {
			return fmt.Errorf("write %s: %w", artifact.name, err)
		}
	}
	return nil
}

func atomicWrite(path string, data []byte) error {
	existing, err := os.ReadFile(path)
	if err == nil && bytes.Equal(existing, data) {
		return os.Chmod(path, 0600)
	}
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(path), ".commissioning-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err := file.Write(data); err != nil {
		file.Close()
		return err
	}
	if err := file.Sync(); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), path)
}

func cString(value string) string {
	var encoded strings.Builder
	encoded.WriteByte('"')
	for _, b := range []byte(value) {
		switch {
		case b == '"' || b == '\\':
			encoded.WriteByte('\\')
			encoded.WriteByte(b)
		case b >= 32 && b <= 126:
			encoded.WriteByte(b)
		default:
			fmt.Fprintf(&encoded, "\\%03o", b)
		}
	}
	encoded.WriteByte('"')
	return encoded.String()
}

func makeHeader(device identity) []byte {
	var header strings.Builder
	header.WriteString("// Generated by tools/commissioning. Keep with this device's firmware.\n#pragma once\n#include <stdint.h>\n\n")
	stringsToDefine := []struct{ name, value string }{
		{"VENDOR_NAME", device.VendorName}, {"PRODUCT_NAME", device.ProductName}, {"NODE_LABEL", device.NodeLabel},
		{"SOFTWARE_VERSION_STRING", device.SoftwareVersionString}, {"HARDWARE_VERSION_STRING", device.HardwareVersionString},
		{"SERIAL_NUMBER", device.SerialNumber},
	}
	for _, item := range stringsToDefine {
		fmt.Fprintf(&header, "#define DISPLAY_MATTER_%s %s\n", item.name, cString(item.value))
	}
	fmt.Fprintf(&header, "#define DISPLAY_MATTER_VENDOR_ID 0x%04xu\n#define DISPLAY_MATTER_PRODUCT_ID 0x%04xu\n#define DISPLAY_MATTER_SOFTWARE_VERSION %du\n#define DISPLAY_MATTER_HARDWARE_VERSION %du\n#define DISPLAY_MATTER_DISCRIMINATOR %du\n#define DISPLAY_MATTER_SPAKE2P_ITERATIONS %du\n", device.VendorID, device.ProductID, device.SoftwareVersion, device.HardwareVersion, device.Discriminator, device.SPAKE2PIterations)
	arrays := []struct {
		name string
		data []byte
	}{{"SPAKE2P_SALT", device.SPAKE2PSalt}, {"SPAKE2P_VERIFIER", device.SPAKE2PVerifier}, {"ROTATING_ID", device.RotatingID}}
	for _, array := range arrays {
		fmt.Fprintf(&header, "\nstatic const uint8_t DISPLAY_MATTER_%s[] = {", array.name)
		for i, b := range array.data {
			if i%12 == 0 {
				header.WriteString("\n    ")
			}
			fmt.Fprintf(&header, "0x%02x, ", b)
		}
		header.WriteString("\n};\n")
	}
	return []byte(header.String())
}

func makeQRCode(payload string) ([]byte, error) {
	code, err := qr.Encode(payload, qr.M, qr.AlphaNumeric)
	if err != nil {
		return nil, err
	}
	// Add the four-module quiet zone required by QR and scale by an integer
	// factor so every module remains sharp and uniformly sized.
	const scale = 12
	const quiet = 4
	dimension := code.Bounds().Dx()
	picture := image.NewGray(image.Rect(0, 0, (dimension+2*quiet)*scale, (dimension+2*quiet)*scale))
	for i := range picture.Pix {
		picture.Pix[i] = 255
	}
	for y := 0; y < dimension; y++ {
		for x := 0; x < dimension; x++ {
			value := color.GrayModel.Convert(code.At(x, y)).(color.Gray).Y
			for dy := 0; dy < scale; dy++ {
				start := ((y+quiet)*scale+dy)*picture.Stride + (x+quiet)*scale
				for dx := 0; dx < scale; dx++ {
					picture.Pix[start+dx] = value
				}
			}
		}
	}
	var output bytes.Buffer
	err = png.Encode(&output, picture)
	return output.Bytes(), err
}
