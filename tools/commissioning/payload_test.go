package main

import (
	"bytes"
	"encoding/base64"
	"testing"
)

// Vectors from connectedhomeip/src/setup_payload/tests/TestQRCode.cpp and
// TestHelpers.h, independent of this implementation's packing/encoding.
func TestCHIPBase38Vectors(t *testing.T) {
	for _, vector := range []struct {
		input []byte
		want  string
	}{
		{nil, ""}, {[]byte{10}, "A0"}, {[]byte{10, 10}, "OT10"},
		{[]byte{10, 10, 10}, "-N.B0"}, {[]byte{10, 10, 40}, "Y6V91"},
		{[]byte{10, 10, 255}, "Q-M08"}, {[]byte{255, 0}, "R600"},
		{[]byte{46, 0, 0}, "81000"},
	} {
		if got := base38(vector.input); got != vector.want {
			t.Errorf("base38(%x) = %q, want %q", vector.input, got, vector.want)
		}
	}
	if got := qrPayload(12, 1, 128, 2048, 1); got != "MT:M5L90MP500K64J00000" {
		t.Fatalf("CHIP QR payload = %q", got)
	}
}

func TestCHIPManualCodes(t *testing.T) {
	// Documented default manual code in connectedhomeip's chip_tool_guide.md.
	if got := manualCode(3840, 20202021); got != "34970112332" {
		t.Fatalf("default manual code = %q", got)
	}
	// TestManualCode.cpp gives the ten payload digits; Verhoeff adds digit 3.
	if got := manualCode(2560, 12345679); got != "24129507533" {
		t.Fatalf("CHIP manual code vector = %q", got)
	}
}

func TestCHIPSPAKE2PVerifier(t *testing.T) {
	// The official test vector in connectedhomeip/scripts/tools/spake2p/README.md
	// and src/protocols/secure_channel/tests/TestPASESession.cpp.
	want, err := base64.StdEncoding.DecodeString("uWFwqugDNGiEck/po7KHwwMwwqZgN10XuyBajPGuyzUEV/iree4lOrao5GuwnlQ65CJzbeUB49s31EH+NEkg0JVI5MGCQGMMT/SRPFNRODm3wH/MBiehuFc6FJ/NH6Rmzw==")
	if err != nil {
		t.Fatal(err)
	}
	got, err := spake2pVerifier(20202021, []byte("SPAKE2P Key Salt"), 1000)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 97 || !bytes.Equal(got, want) {
		t.Fatalf("SPAKE2+ verifier mismatch: got %x, want %x", got, want)
	}
}

func TestInvalidSPAKE2PInputs(t *testing.T) {
	for _, passcode := range []uint32{0, 99999999, 11111111, 22222222, 33333333, 44444444, 55555555, 66666666, 77777777, 88888888, 12345678, 87654321} {
		if _, err := spake2pVerifier(passcode, make([]byte, 32), 10000); err == nil {
			t.Errorf("accepted invalid passcode %d", passcode)
		}
	}
	for _, count := range []int{0, 999, 100001} {
		if _, err := spake2pVerifier(12345679, make([]byte, 32), count); err == nil {
			t.Errorf("accepted invalid iterations %d", count)
		}
	}
	for _, size := range []int{0, 15, 33} {
		if _, err := spake2pVerifier(12345679, make([]byte, size), 10000); err == nil {
			t.Errorf("accepted salt length %d", size)
		}
	}
}
