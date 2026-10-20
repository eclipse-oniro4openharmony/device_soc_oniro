# hybris GNSS — OHOS location on the Halium GNSS stack

MediaTek's GNSS engine (`mnld`, `mtk_agpsd`, `mtk_lbs_service`) and
`android.hardware.gnss-service.mediatek` run inside the androidd
container and publish the standard AIDL **`android.hardware.gnss.IGnss`,
version 3**, instance `default`, on `/dev/binder`. This directory is the
OHOS side of that seam: `vendorGnssAdapter.so`, the vendor library the
stock location HDIs load.

```
app ─ geoLocationManager ─ locationhub (GnssAbility)
        └─ location_host: gnss / agnss / geofence HDIs
             └─ dlopen("vendorGnssAdapter.so"), dlsym("GnssVendorInterface")
                  └─ AIDL IGnss v3 (+ IAGnss, IAGnssRil) over /dev/binder
                       └─ container: GNSS HAL → mnld → /dev/gpsdl0 (MT6637)
```

No upstream code is touched. `device_info.hcs` already declares
`location_host` with all three devices (`preload = 2`, i.e. loaded when
`locationhub` first asks), and the location framework ships in the base
image.

## Layout

| Path | What it is |
|---|---|
| `src/hybris_gnss_adapter.cpp` | `GnssVendorInterface` — the whole vendor ABI (`location_vendor_lib.h`). |
| `src/hybris_gnss_bridge.cpp` | Connection to `IGnss/default`, the wanted-session state, the report dispatcher. |
| `src/hybris_gnss_callbacks.cpp` | `IGnssCallback` / `IAGnssCallback` / `IAGnssRilCallback` → OHOS structs. |
| `src/hybris_gnss_agnss.cpp` | `AGNSS_MODULE_INTERFACE` onto `IAGnss` + `IAGnssRil`. |
| `aidl_gen/` | **Generated** AIDL NDK code for `android.hardware.gnss` v3 — checked in; `regen.sh` documents the two inputs. |
| `test/hybris_gnss_test.cpp` | Probe that drives the installed library through the vendor ABI (`install_enable = false`). |

The libbinder_ndk shim, the vendored NDK headers and the libhybris loader
are the RIL bridge's (`../ril/`), compiled a second time into this
library — `location_host` is its own process and needs its own slot
table. `BUILD.gn` overrides `hybris_ril_log.h`'s domain/tag so those lines
log as `HybrisGnss` too. Everything in `../ril/README.md` about the
servicemanager bootstrap wedge and starting the thread pool late applies
here unchanged, and is handled the same way.

## What is bridged

| OHOS vendor op | AIDL |
|---|---|
| `enableGnss` | records the callbacks; the connect thread already did `setCallback` |
| `setGnssConfigPara` | `setPositionMode` (live sessions re-issue it without restarting) |
| `startGnss` / `stopGnss` | `setPositionMode` + `start` + `startSvStatus` + `startNmea` / the reverse |
| `injectsGnssReferenceInfo` | `injectTime`, `injectLocation`, `injectBestLocation` |
| `removeAuxiliaryData` | `deleteAidingData` (`CELLDB` is bit 11 here, `0x8000` there) |
| AGNSS `setAgnssServer` / `setSetid` / `setAgnssReferenceInfo` / `onNetworkStateChange` | `IAGnss.setServer`, `IAGnssRil.setSetId` / `setRefLocation` / `updateNetworkState` |

Reports: location, satellite status, NMEA, engine status, capabilities,
and the AGNSS set-ID / reference-location / data-link requests.

**Not bridged**, each returning NULL/unsupported, which every HDI caller
handles: geofencing (the MTK HAL does not advertise
`CAPABILITY_GEOFENCING`), GNSS measurements, network-initiated
notifications, batching (`GNSS_CACHE` — the framework only asks on
wearables) and PSDS injection (MTK downloads EPO itself).

## Design points

**Desired state, replayed.** The framework may enable and start a
session before the container's HAL is reachable — the location switch
persists across reboots, so `locationhub` can come up early. Every op
records what is wanted; the connect thread applies it on connect, and a
death notification on the HAL binder sends it back round to reconnect
and replay. `enableGnss` waits up to 5 s for the first connect and then
accepts anyway; only a container that does not *declare*
`IGnss/default` (`AServiceManager_isDeclared`) makes it fail. That
declaration is also the per-device gate: there is no parameter to set,
and a vendor with only the HIDL GNSS HAL gets a clean "unavailable".

**Reports go through our own thread.** The stock HDI calls
`enableGnss()` while holding the mutex its report callbacks take, and
the MTK HAL answers `setCallback()`/`start()` with synchronous
callbacks. Delivered inline on the binder thread that is a deadlock
across the binder, so every report is copied and queued to one
dispatcher thread (bounded at 512 entries; NMEA alone runs at ~23/s).

**`-z nodelete`.** The HDI dlcloses the vendor library when the
framework unloads its devices, but the container's binder threads keep
running our callback code; the library is never unmapped.

## Known gaps outside this library

* `GnssAbility::SetGeofenceCallback` treats an HDF status as a bool
  (`if (!ret) return false;`, `gnss_ability.cpp`), so it reports
  failure exactly when the HDI succeeds and logs `ConnectGeofenceHdi
  fail` on every connect. Harmless: GNSS proceeds regardless.
* Assistance depends on three things outside this directory, all in
  `device/board/oniro/hybris_generic/launcher/androidd.c`: the guest's
  `dnsproxyd` (container DNS), its `cacerts` (TLS for the EPO/QEPO/MTKNAV
  downloads) and the persistent `/data/vendor/gps` + `agps_supl`. Without
  them every start is a factory cold start with no EPO. Also needed: a
  correct clock (`persist.time.auto_time` ON), because MNL seeds its search
  from system time. The SUPL server and mode come from
  `const.location.*` in `vendor/oniro/hybris_generic/etc/param/hybris_native.para`.
* The framework does not set up AGNSS data links (its
  `RequestSetUpAgnssDataLink` only logs), so the bridge answers the
  HAL's request itself with `dataConnOpen` on the default network.
* `requestGlobalSwitch(LOCATION)` fails with "Invalid parameter" on this
  build; turn location on in Settings → Privacy & security → location
  services.

## Building and probing

```bash
ninja -w dupbuild=warn vendorGnssAdapter hybris_gnss_test
hdc file send out/hybris_generic/oniro_soc_products/hybris_generic_soc/hybris_gnss_test /data/local/tmp/
hdc shell "chmod 755 /data/local/tmp/hybris_gnss_test && \
           /data/local/tmp/hybris_gnss_test 120 /vendor/lib64/vendorGnssAdapter.so"
```

The probe takes over the HAL's single callback slot while it runs; turn
the location switch off first. Afterwards `location_host` re-claims the
slot at its next session start (the bridge calls `setCallback` before
every `start`), so a probe run leaves it deaf only until then. It prints engine status, a satellite summary every 5 s,
fixes and the first NMEA sentence.

Runtime logs:

```bash
hdc shell "timeout 20 hilog -x | grep HybrisGnss"
# the engine itself, from inside the container:
hdc shell 'nsenter -t $(pidof mnld) -m -p -- /system/bin/logcat -d | grep -E "MNLD|GnssAidl|gps_controlller"'
# the kernel GNSS link (DSP state, DMA traffic):
hdc shell 'dmesg | grep GDL'
```

An app-level check is the GNSS tab of the Oniro HW Tester app.
