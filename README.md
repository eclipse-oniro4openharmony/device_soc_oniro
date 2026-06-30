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

The full bring-up, boot architecture, and per-phase documentation live in the
board repository, under `device/board/oniro/docs/hybris_generic/README.md`.

### `x86_general` — QEMU / x86 emulator

GPU adaptation for the Oniro emulator target, based on the Mesa graphics stack.
See the build instructions in the board repository's README.

## Building

These repositories are assembled into a full Oniro source tree via the Oniro
manifest; you do not clone them individually:

```bash
repo init -u https://github.com/eclipse-oniro4openharmony/manifest.git -b OpenHarmony-6.1-Release -m oniro.xml --no-repo-verify
repo sync -c
repo forall -c 'git lfs pull'
```

## Repository layout

| Path | Description |
| --- | --- |
| `hybris_generic/hardware/` | libhybris-backed HDI/HAL implementations (display, audio) for Volla devices |
| `x86_general/hardware/gpu/` | Mesa-based GPU stack for the QEMU emulator |

## License

Released under the Apache License, Version 2.0. See [LICENSE](./LICENSE) and
[NOTICE](./NOTICE).
