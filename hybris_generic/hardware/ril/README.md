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
| `src/hybris_ril_network.cpp` | `networkOps` + `IRadioNetworkResponse`/`Indication`, including the three signal-strength encodings. |
| `src/hybris_ril_sms.cpp` | `smsOps` + `IRadioMessagingResponse`/`Indication`. |
| `src/hybris_ril_presence.cpp` | Stub registration for the interfaces we do not implement yet. **Not optional** — see below. |
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

To move a package to a different frozen version, edit the `V2_PKGS` /
`V1_PKGS` lists in `regen.sh` (each version group is a separate `aidl`
invocation, because `--version` applies to a whole run), re-run it, and
refresh `binder_ndk_shim/binder_ndk_symbols.txt` from the device (`nm -D
--defined-only libbinder_ndk.so`, filtered to the
`A{IBinder,Parcel,Status,ServiceManager}_`/`ABinderProcess_` prefixes)
followed by `gen_trampolines.sh`.

## Register on all seven interfaces, always

MTK's rild does not treat the `IRadio*` interfaces independently. Every
AOSP `setResponseFunctions` goes through
`rilAidlUtils::checkIfSetAllAospResponseDone()` in `librilfusion.so`,
which is an AND over **seven** per-slot types — data, messaging, modem,
network, sim, voice, ims — and only when all seven have registered does
rild consider the framework connected. Registering only the interfaces
you use leaves inbound SMS queued and NACKed forever while modem, SIM and
network work normally, because none of those consult that state.

`hybris_ril_presence.cpp` therefore registers the three we do not
implement yet with the generated `Default` handlers. When data (R5) and
voice (R6) land, they must **replace** their stub, not add a second
registration — `setResponseFunctions` overwrites.

## Two ways this seam wedges

Both cost a core with nothing in our own logs, so `dumpcatcher -p <pid>`
is the first tool to reach for:

* **`android::defaultServiceManager()`** caches its proxy under
  `std::call_once` and retries internally until it gets a context object.
  Called before the container's servicemanager owns the context manager,
  it never finishes and the once-flag blocks any retry — for the life of
  the process. Every `AServiceManager_*` entry point goes through it.
  Hence `WaitForServiceManager()` (gate on the *AIDL* servicemanager, not
  on `android.composer.ready`, which proves the HIDL one) plus
  `ProbeServiceManager()` (first contact on a thread we can abandon,
  `_exit(1)` on timeout so hdf_devmgr restarts us).
* **Joining the binder thread pool early.** Start it only once a service
  handle is in hand — it exists to receive the callbacks
  `setResponseFunctions` installs.

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
