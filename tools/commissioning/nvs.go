package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"strings"
	"unicode"
	"unicode/utf8"
)

const (
	provisioningRecordSize = 200
	provisioningNVSSize    = 0x6000
	nvsPageSize            = 4096
	nvsEntrySize           = 32
	nvsEntriesOffset       = 64
)

// makeProvisioningRecord matches the portable AMPI schema in
// main/matter_provisioning.c; it contains the verifier, never the setup passcode.
func makeProvisioningRecord(device identity) ([]byte, error) {
	if device.Discriminator > 4095 || device.SPAKE2PIterations < 1000 || device.SPAKE2PIterations > 100000 {
		return nil, fmt.Errorf("invalid provisioning discriminator or iteration count")
	}
	if len(device.SerialNumber) == 0 || len(device.SerialNumber) > 32 ||
		!utf8.ValidString(device.SerialNumber) || strings.IndexFunc(device.SerialNumber, unicode.IsControl) != -1 {
		return nil, fmt.Errorf("provisioning serial must contain 1 to 32 UTF-8 bytes without control characters")
	}
	if len(device.SPAKE2PSalt) != 32 || len(device.SPAKE2PVerifier) != 97 || len(device.RotatingID) != 16 {
		return nil, fmt.Errorf("invalid provisioning salt, verifier, or rotating ID length")
	}
	record := make([]byte, provisioningRecordSize)
	copy(record, "AMPI")
	binary.LittleEndian.PutUint16(record[4:6], 1)
	binary.LittleEndian.PutUint16(record[6:8], provisioningRecordSize)
	binary.LittleEndian.PutUint16(record[8:10], device.Discriminator)
	binary.LittleEndian.PutUint32(record[10:14], uint32(device.SPAKE2PIterations))
	binary.LittleEndian.PutUint16(record[14:16], device.VendorID)
	binary.LittleEndian.PutUint16(record[16:18], device.ProductID)
	record[18] = byte(len(device.SerialNumber))
	copy(record[19:51], device.SerialNumber)
	copy(record[51:83], device.SPAKE2PSalt)
	copy(record[83:180], device.SPAKE2PVerifier)
	copy(record[180:196], device.RotatingID)
	binary.LittleEndian.PutUint32(record[196:200], crc32.ChecksumIEEE(record[:196]))
	return record, nil
}

// ESP-IDF NVS uses the reflected IEEE CRC with a supplied 0xffffffff seed,
// unlike AMPI's ordinary CRC32. This matches the official partition generator's
// zlib.crc32(data, 0xffffffff), including for page, entry, and blob-data CRCs.
func nvsCRC(data []byte) uint32 {
	return crc32.Update(0xffffffff, crc32.IEEETable, data)
}

func initializeNVSEntry(entry []byte, namespace, kind, span, chunk byte, key string) {
	entry[0], entry[1], entry[2], entry[3] = namespace, kind, span, chunk
	clear(entry[8:24])
	copy(entry[8:24], key)
}

func finishNVSEntry(entry []byte) {
	crc := nvsCRC(entry[:4])
	crc = crc32.Update(crc, crc32.IEEETable, entry[8:32])
	binary.LittleEndian.PutUint32(entry[4:8], crc)
}

// makeProvisioningNVS emits one active NVS V2 page with a namespace, one blob
// chunk, and its blob index. Remaining pages stay erased for normal NVS use and
// garbage collection. The layout follows ESP-IDF's esp_idf_nvs_partition_gen
// Page.set_header/write_varlen_binary_data/write_primitive_data format.
func makeProvisioningNVS(device identity) ([]byte, error) {
	record, err := makeProvisioningRecord(device)
	if err != nil {
		return nil, err
	}
	partition := bytes.Repeat([]byte{0xff}, provisioningNVSSize)
	page := partition[:nvsPageSize]
	binary.LittleEndian.PutUint32(page[0:4], 0xfffffffe) // Active page.
	binary.LittleEndian.PutUint32(page[4:8], 0)          // First sequence number.
	page[8] = 0xfe                                       // NVS V2.
	binary.LittleEndian.PutUint32(page[28:32], nvsCRC(page[4:28]))
	entryAt := func(index int) []byte {
		offset := nvsEntriesOffset + index*nvsEntrySize
		return page[offset : offset+nvsEntrySize]
	}

	namespace := entryAt(0)
	initializeNVSEntry(namespace, 0, 0x01, 1, 0xff, "matter_prov") // u8 namespace index.
	namespace[24] = 1
	finishNVSEntry(namespace)

	dataEntries := (len(record) + nvsEntrySize - 1) / nvsEntrySize
	chunk := entryAt(1)
	initializeNVSEntry(chunk, 1, 0x42, byte(dataEntries+1), 0, "identity")
	binary.LittleEndian.PutUint16(chunk[24:26], uint16(len(record)))
	binary.LittleEndian.PutUint32(chunk[28:32], nvsCRC(record))
	finishNVSEntry(chunk)
	copy(page[nvsEntriesOffset+2*nvsEntrySize:], record)

	indexEntry := 2 + dataEntries
	index := entryAt(indexEntry)
	initializeNVSEntry(index, 1, 0x48, 1, 0xff, "identity")
	binary.LittleEndian.PutUint32(index[24:28], uint32(len(record)))
	index[28], index[29] = 1, 0 // One chunk, starting at chunk zero.
	finishNVSEntry(index)

	// Each entry's two-bit state changes from 11 (empty) to 10 (written).
	for i := 0; i <= indexEntry; i++ {
		page[32+i/4] &^= 1 << uint((i%4)*2)
	}
	return partition, nil
}
