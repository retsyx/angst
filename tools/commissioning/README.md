# Matter commissioning generator

Generates per-device Matter credentials, pairing artifacts, and provisioning/build inputs.

- **`qrcode.png`**: scan it in your Matter controller's Add Accessory flow.
- **`onboarding.txt`**: the manual pairing code and QR payload.
- **`identity.json`**: the private host backup of initial credentials and accessory metadata.
- **`provisioning-nvs.bin`**: the private device identity image for the USB flash helper.
- **`matter_identity.h`** and **`matter_identity.cmake`**: build inputs.

## Command-line options

Run `angst-commissioner --help` for usage. The tool only writes to its `--out` folder; it does not
read or modify firmware files. Relative paths are resolved from the current working directory.

| Flag | Default for a new identity |
| --- | --- |
| `--out` | `generated/matter`, relative to the working directory |
| `--vendor-name` | `Angst` |
| `--product-name` | `Angst Display` |
| `--node-label` | `Angst` |
| `--vendor-id` | `0xfff1` |
| `--product-id` | `0x8000` |
| `--software-version` | `2` |
| `--software-version-string` | `2.0.0` |
| `--hardware-version` | `1` |
| `--hardware-version-string` | `T-Display-S3 AMOLED` |
| `--serial-number` | Random `ANGST-` serial |
| `--regenerate` | Disabled |

The vendor and product IDs must match the Device Attestation Certificate and Certification
Declaration. The defaults match the SDK's test attestation identity; changing these numbers alone
does not install production attestation credentials. Names and firmware/hardware versions affect
local source builds; generating a provisioning image does not change a prebuilt release's compiled
metadata.

## Reusing or regenerating output

The tool loads an existing `identity.json`, preserves its credentials, and recreates derived files.
Only explicitly supplied metadata flags change existing host metadata. A malformed `identity.json`
causes an error rather than silently replacing host credentials.

Use `--regenerate` only when intentionally creating new host identity files. It replaces the
pairing credentials and rotating device ID, and chooses a new serial unless `--serial-number` is
supplied. Metadata also returns to the current CLI defaults unless its flags are supplied again.

## Build and test the tool

With Go 1.24 or newer, run from `tools/commissioning/`:

```sh
go build -mod=vendor -o angst-commissioner .
go test -mod=vendor ./...
go vet -mod=vendor ./...
```

## Implementation

Random values come from Go's cryptographic random source. The generator excludes Matter's invalid
passcodes and the SDK's public default passcode and discriminator. QR images include a four-module
quiet zone. Neither the generated firmware header nor the provisioning image includes the plaintext
passcode.

Matter QR payload packing, Base38 encoding, manual-code Verhoeff checksum, and SPAKE2+ derivation
follow the [connectedhomeip setup payload
implementation](https://github.com/project-chip/connectedhomeip/tree/master/src/setup_payload) and
[SPAKE2+ verifier
implementation](https://github.com/project-chip/connectedhomeip/blob/master/src/crypto/CHIPCryptoPAL.cpp).
Tests use independent official CHIP QR/Base38/manual and SPAKE2+ vectors. PNG rendering uses the
vendored [boombuler/barcode v1.1.0](https://github.com/boombuler/barcode/releases/tag/v1.1.0) QR
encoder under its included MIT license.
