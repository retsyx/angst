package main

import (
	"bytes"
	"encoding/json"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func readIdentity(t *testing.T, out string) identity {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(out, "identity.json"))
	if err != nil {
		t.Fatal(err)
	}
	var device identity
	if err := json.Unmarshal(data, &device); err != nil {
		t.Fatal(err)
	}
	return device
}

func invoke(t *testing.T, out string, args ...string) {
	t.Helper()
	if err := run(append([]string{"--out", out}, args...), io.Discard, io.Discard); err != nil {
		t.Fatal(err)
	}
}

func TestPersistedIdentityAndMetadataUpgrade(t *testing.T) {
	out := t.TempDir()
	invoke(t, out, "--vendor-name", "Example Manufacturer", "--product-name", "Display", "--node-label", "Desk", "--serial-number", "DISPLAY-0001")
	first := readIdentity(t, out)
	if err := validateIdentity(first); err != nil {
		t.Fatal(err)
	}
	if first.Passcode == 20202021 || first.Discriminator == 3840 || len(first.SPAKE2PSalt) != 32 || len(first.SPAKE2PVerifier) != 97 || len(first.RotatingID) != 16 {
		t.Fatal("generated identity has invalid or default credentials")
	}
	paths := []string{"identity.json", "provisioning-nvs.bin", "matter_identity.h", "matter_identity.cmake", "onboarding.txt", "qrcode.png"}
	before := make(map[string][]byte)
	for _, path := range paths {
		data, err := os.ReadFile(filepath.Join(out, path))
		if err != nil {
			t.Fatal(err)
		}
		before[path] = data
		info, err := os.Stat(filepath.Join(out, path))
		if err != nil || info.Mode().Perm() != 0600 {
			t.Fatalf("%s permissions must be 0600", path)
		}
	}
	invoke(t, out)
	if second := readIdentity(t, out); !reflect.DeepEqual(first, second) {
		t.Fatal("ordinary rebuild changed the persisted identity")
	}
	for path, expected := range before {
		data, err := os.ReadFile(filepath.Join(out, path))
		if err != nil || !bytes.Equal(data, expected) {
			t.Fatalf("ordinary rebuild changed %s", path)
		}
	}
	// Missing derived files must be recovered using the same credentials.
	if err := os.Remove(filepath.Join(out, "matter_identity.h")); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(filepath.Join(out, "provisioning-nvs.bin")); err != nil {
		t.Fatal(err)
	}
	invoke(t, out, "--software-version", "3", "--software-version-string", "3.1.0")
	upgraded := readIdentity(t, out)
	if upgraded.SoftwareVersion != 3 || upgraded.SoftwareVersionString != "3.1.0" || upgraded.VendorName != first.VendorName {
		t.Fatal("explicit metadata update failed")
	}
	upgraded.SoftwareVersion, upgraded.SoftwareVersionString = first.SoftwareVersion, first.SoftwareVersionString
	if !reflect.DeepEqual(first, upgraded) {
		t.Fatal("firmware upgrade changed pairing credentials or serial number")
	}
	nvs, err := os.ReadFile(filepath.Join(out, "provisioning-nvs.bin"))
	if err != nil || !bytes.Equal(nvs, before["provisioning-nvs.bin"]) {
		t.Fatal("version metadata update changed the provisioning identity or failed to restore its NVS image")
	}
	invoke(t, out, "--regenerate")
	fresh := readIdentity(t, out)
	if bytes.Equal(fresh.SPAKE2PSalt, first.SPAKE2PSalt) || bytes.Equal(fresh.RotatingID, first.RotatingID) || fresh.SerialNumber == first.SerialNumber {
		t.Fatal("explicit regeneration did not replace the identity")
	}
}

func TestCLIRejectsBadMetadata(t *testing.T) {
	for _, args := range [][]string{
		{"--vendor-id", "0x10000"}, {"--vendor-id", "0xfff5"}, {"--vendor-id", "0"},
		{"--product-id", "0"}, {"--product-id", "65536"}, {"--hardware-version", "65536"},
		{"--software-version", "4294967296"}, {"--software-version", "0"},
		{"--vendor-name", strings.Repeat("a", 33)}, {"--node-label", "bad\nlabel"},
		{"--software-version-string", "bad;${CMAKE_CURRENT_SOURCE_DIR}"},
		{"--software-version-string", strings.Repeat("a", 32)}, {"--product-name", ""},
		{"unexpected"},
	} {
		t.Run(strings.Join(args, " "), func(t *testing.T) {
			out := t.TempDir()
			if err := run(append([]string{"--out", out}, args...), io.Discard, io.Discard); err == nil {
				t.Fatal("accepted invalid metadata")
			}
			if _, err := os.Stat(filepath.Join(out, "identity.json")); !os.IsNotExist(err) {
				t.Fatal("invalid metadata created an identity")
			}
		})
	}
}

func TestCorruptIdentityRequiresExplicitRegeneration(t *testing.T) {
	out := t.TempDir()
	invoke(t, out)
	device := readIdentity(t, out)
	device.SPAKE2PVerifier[0] ^= 1
	data, err := json.Marshal(device)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(out, "identity.json"), data, 0600); err != nil {
		t.Fatal(err)
	}
	if err := run([]string{"--out", out}, io.Discard, io.Discard); err == nil || !strings.Contains(err.Error(), "does not match") {
		t.Fatal("corrupt identity was silently accepted or replaced")
	}
	unchanged, err := os.ReadFile(filepath.Join(out, "identity.json"))
	if err != nil || !bytes.Equal(unchanged, data) {
		t.Fatal("failed generation changed the stored credentials")
	}
	invoke(t, out, "--regenerate")
}

func TestQRCodeHasQuietZoneAndSharpModules(t *testing.T) {
	data, err := makeQRCode("MT:M5L90MP500K64J00000")
	if err != nil {
		t.Fatal(err)
	}
	picture, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	dimension := picture.Bounds().Dx()
	if dimension != picture.Bounds().Dy() || dimension%12 != 0 {
		t.Fatal("QR image dimensions do not preserve whole modules")
	}
	for y := 0; y < dimension; y++ {
		for x := 0; x < dimension; x++ {
			v := color.GrayModel.Convert(picture.At(x, y)).(color.Gray).Y
			if v != 0 && v != 255 {
				t.Fatalf("QR pixel (%d, %d) is antialiased", x, y)
			}
			if x < 48 || y < 48 || x >= dimension-48 || y >= dimension-48 {
				if v != 255 {
					t.Fatalf("QR quiet zone contains a dark pixel at (%d, %d)", x, y)
				}
			}
		}
	}
	if v := color.GrayModel.Convert(picture.At(48, 48)).(color.Gray).Y; v != 0 {
		t.Fatal("top-left finder pattern is absent")
	}
}

func TestHeaderEscapesCStrings(t *testing.T) {
	if got := cString("Café \"Lamp\" \\ é"); got != `"Caf\303\251 \"Lamp\" \\ \303\251"` {
		t.Fatalf("invalid C string escaping: %s", got)
	}
}
