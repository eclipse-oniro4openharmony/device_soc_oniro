# Oniro SoC Adaptations

This repository contains SoC-level adaptations for the System-on-Chips supported
by the Oniro Project. It provides the hardware-facing HDI/HAL implementations
(display, audio, GPU, …) that sit between the OpenHarmony framework and the
silicon, for both physical devices and the virtual emulator.

## Targets

### `hybris_generic` — MediaTek SoCs (Volla devices)

SoC adaptation for running **OpenHarmony natively** on MediaTek-based Volla
hardware (Volla X23, MT6789; Volla Tablet, MT8781) by reusing the device's
Android vendor stack through **libhybris**. This layer hosts the vendor display
interface (VDI) and audio HAL implementations that bridge OpenHarmony HDIs onto
the Android vendor blobs.

### `emulator` — the QEMU emulator (x86_64 and arm64)

SoC adaptation for the Oniro emulator products `emulator_x86_64` and
`emulator_arm64`: display composer, the Mesa graphics stack, and simulated
sensors, GNSS, modem and camera. The prebuilt GPU libraries are cross-compiled
from Mesa source, one set per architecture; their provenance and rebuild recipe
are documented in
[`emulator/hardware/gpu/x86_64/PROVENANCE.md`](emulator/hardware/gpu/x86_64/PROVENANCE.md)
and
[`emulator/hardware/gpu/arm64/PROVENANCE.md`](emulator/hardware/gpu/arm64/PROVENANCE.md).

## Building

These repositories are assembled into a full Oniro source tree via the Oniro
manifest; you do not clone them individually:

```bash
repo init -u https://github.com/eclipse-oniro4openharmony/manifest.git -b OpenHarmony-7.0-Release -m oniro.xml --no-repo-verify
repo sync -c
repo forall -c 'git lfs pull'
```

## Repository layout

| Path | Description |
| --- | --- |
| `hybris_generic/hardware/` | libhybris-backed HDI/HAL implementations (display, audio) for Volla devices |
| `emulator/hardware/` | HDI/HAL implementations for the QEMU emulator (display, Mesa GPU stack, simulated devices) |

## License

Released under the Apache License, Version 2.0. See [LICENSE](./LICENSE) and
[NOTICE](./NOTICE).
