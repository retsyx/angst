package main

import (
	"crypto/elliptic"
	"crypto/pbkdf2"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math/big"
	"strings"
)

const maxPasscode = 99999998

// Matter SetupPayload::IsValidSetupPIN excludes these values. The generator
// additionally excludes the SDK's public development passcode, 20202021.
func validPasscode(passcode uint32) bool {
	if passcode == 0 || passcode > maxPasscode || passcode == 12345678 || passcode == 87654321 {
		return false
	}
	for repeated := uint32(11111111); repeated <= 88888888; repeated += 11111111 {
		if passcode == repeated {
			return false
		}
	}
	return true
}

// spake2pVerifier follows connectedhomeip/src/crypto/CHIPCryptoPAL.cpp:
// derive 80 bytes from the little-endian PIN, reduce the two 40-byte big-endian
// integers modulo the P-256 order, and serialize w0 || (w1 * G).
func spake2pVerifier(passcode uint32, salt []byte, iterations int) ([]byte, error) {
	if !validPasscode(passcode) {
		return nil, fmt.Errorf("invalid Matter passcode")
	}
	if len(salt) < 16 || len(salt) > 32 {
		return nil, fmt.Errorf("SPAKE2+ salt must contain 16 to 32 bytes")
	}
	if iterations < 1000 || iterations > 100000 {
		return nil, fmt.Errorf("SPAKE2+ iterations must be between 1000 and 100000")
	}
	var pin [4]byte
	binary.LittleEndian.PutUint32(pin[:], passcode)
	ws, err := pbkdf2.Key(sha256.New, string(pin[:]), salt, iterations, 80)
	if err != nil {
		return nil, err
	}
	curve := elliptic.P256()
	w0 := new(big.Int).Mod(new(big.Int).SetBytes(ws[:40]), curve.Params().N)
	w1 := new(big.Int).Mod(new(big.Int).SetBytes(ws[40:]), curve.Params().N)
	if w0.Sign() == 0 || w1.Sign() == 0 {
		return nil, fmt.Errorf("SPAKE2+ derived a zero scalar; regenerate the identity")
	}
	verifier := make([]byte, 32, 97)
	w0.FillBytes(verifier)
	x, y := curve.ScalarBaseMult(w1.Bytes())
	return append(verifier, elliptic.Marshal(curve, x, y)...), nil
}

func base38(data []byte) string {
	const alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-."
	var encoded strings.Builder
	for len(data) > 0 {
		count := min(len(data), 3)
		var value uint32
		for i := 0; i < count; i++ {
			value |= uint32(data[i]) << (8 * i)
		}
		for i := 0; i < []int{0, 2, 4, 5}[count]; i++ {
			encoded.WriteByte(alphabet[value%38])
			value /= 38
		}
		data = data[count:]
	}
	return encoded.String()
}

func qrPayload(vendorID, productID uint16, discriminator uint16, passcode uint32, rendezvous uint8) string {
	var data [11]byte
	offset := 0
	put := func(value uint32, width int) {
		for bit := 0; bit < width; bit++ {
			if value&(1<<bit) != 0 {
				data[offset/8] |= 1 << (offset % 8)
			}
			offset++
		}
	}
	put(0, 3) // QR version
	put(uint32(vendorID), 16)
	put(uint32(productID), 16)
	put(0, 2) // Standard commissioning flow
	put(uint32(rendezvous), 8)
	put(uint32(discriminator), 12)
	put(passcode, 27)
	put(0, 4) // Padding
	return "MT:" + base38(data[:])
}

func manualCode(discriminator uint16, passcode uint32) string {
	short := discriminator >> 8
	chunk1 := short >> 2
	chunk2 := (uint32(short&3) << 14) | (passcode & 0x3fff)
	chunk3 := passcode >> 14
	payload := fmt.Sprintf("%d%05d%04d", chunk1, chunk2, chunk3)
	return payload + string(verhoeffDigit(payload))
}

// The decimal Verhoeff tables match connectedhomeip's Verhoeff10.cpp.
var verhoeffMultiply = [10][10]byte{
	{0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
	{1, 2, 3, 4, 0, 6, 7, 8, 9, 5},
	{2, 3, 4, 0, 1, 7, 8, 9, 5, 6},
	{3, 4, 0, 1, 2, 8, 9, 5, 6, 7},
	{4, 0, 1, 2, 3, 9, 5, 6, 7, 8},
	{5, 9, 8, 7, 6, 0, 4, 3, 2, 1},
	{6, 5, 9, 8, 7, 1, 0, 4, 3, 2},
	{7, 6, 5, 9, 8, 2, 1, 0, 4, 3},
	{8, 7, 6, 5, 9, 3, 2, 1, 0, 4},
	{9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
}

func verhoeffDigit(payload string) byte {
	permutation := [10]byte{1, 5, 7, 6, 2, 8, 3, 0, 9, 4}
	c := byte(0)
	for i := 1; i <= len(payload); i++ {
		value := payload[len(payload)-i] - '0'
		for p := 0; p < i%8; p++ {
			value = permutation[value]
		}
		c = verhoeffMultiply[c][value]
	}
	if c < 5 {
		c = (5 - c) % 5
	}
	return '0' + c
}
