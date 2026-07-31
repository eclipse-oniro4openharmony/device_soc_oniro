# hybris RIL — OHOS telephony on the Halium radio stack

The MediaTek modem and `mtkfusionrild` run inside the androidd container
and publish the standard AIDL `android.hardware.radio.*` HAL, **version
2**, on `/dev/binder`. Everything here is the OHOS side of that seam.

Design, phases and the implementation log live in
`device/board/oniro/docs/hybris_generic/cellular_enablement_plan.md`.

## Layout

| Path | What it is |
|---|---|
| `src/hybris_ril_vendor.cpp` | `RilInitOps()` — the entire vendor-RIL ABI. Loaded by `riladapter_host` via `const.sys.radio.vendorlib.path`. |
| `src/hybris_ril_bridge.cpp` | Connection to the AIDL services, serial ⇄ `ReqDataInfo` map, report helpers, `RadioError` → `HRilErrNumber`. |
| `src/hybris_ril_modem.cpp` | `modemOps` + `IRadioModemResponse`/`Indication`. |
| `src/hybris_ril_sim.cpp` | `simOps` floor + `IRadioSimResponse`/`Indication`. |
| `src/hybris_ril_dl.cpp` | libhybris loader (system-first search path). |
| `binder_ndk_shim/` | 142 aarch64 tail-jump trampolines that give the generated code libbinder_ndk's C ABI, bound at load time through hybris. |
| `aidl_gen/` | **Generated** AIDL NDK client/server code — checked in. |
| `android_binder_ndk/` | **Vendored** AOSP libbinder_ndk headers + a musl compat shim. |
| `test/hybris_ril_test.cpp` | Standalone IRadio v2 probe (`install_enable = false`). |

## Third-party provenance

Both vendored trees are Apache-2.0, from AOSP `android14-release`:

* `aidl_gen/` — output of `aidl --lang=ndk --structured --stability=vintf
  --version=2` over the **frozen v2 snapshots** in
  `platform/hardware/interfaces` → `radio/aidl/aidl_api/*/2/`. Checked in
  so the OHOS build needs neither the `aidl` compiler nor an AOSP
  checkout. Regenerate with `aidl_gen/regen.sh` (see its header for the
  two inputs); it also rewrites `sources.gni`, which the build imports
  because GN cannot glob.
* `android_binder_ndk/include_{ndk,cpp,platform}/` — verbatim from
  `platform/frameworks/native` → `libs/binder/ndk/`, minus the JNI and
  libbinder-C++ headers we cannot use. `binder_ndk_compat.h` is ours: it
  is force-included ahead of them and neutralises bionic-isms
  (`__INTRODUCED_IN`, `__ANDROID_API__`) and turns on
  `BINDER_STABILITY_SUPPORT`. No ABI is changed — the libbinder_ndk C ABI
  is deliberately opaque and every call is forwarded into the container's
  own implementation.

To move to a different frozen version, change `VERSION` in `regen.sh`,
re-run it, and refresh `binder_ndk_shim/binder_ndk_symbols.txt` from the
device (`nm -D --defined-only libbinder_ndk.so`, filtered to the
`A{IBinder,Parcel,Status,ServiceManager}_`/`ABinderProcess_` prefixes)
followed by `gen_trampolines.sh`.

## Per-device gating

The library ships on every hybris_generic image but is inert unless
`const.sys.radio.vendorlib.path` names it, which only `init.ansuz.cfg`
does. Do **not** declare that parameter in a `.para` file: `const.*` is
write-once and `.para` loads first, so an empty default silently wins.

## Building and running the probe

```bash
ninja -w dupbuild=warn oniro_soc_products/hybris_generic_soc/hybris_ril_test
hdc file send out/.../hybris_ril_test /data/hybris_ril_test
hdc shell "chmod 755 /data/hybris_ril_test && /data/hybris_ril_test slot1"
```

Runtime logs use their own hilog domain:

```bash
hdc shell "hilog -x | grep HybrisRil"
```

Note that the first-boot lines are lost — `riladapter_host` loads the
library before hilogd is accepting. `kill -9` the host (hdf_devmgr
restarts it) to see the full bring-up sequence.
