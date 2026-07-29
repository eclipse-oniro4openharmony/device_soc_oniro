/*
 * Copyright (c) 2026 Oniro Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * libaudio_primary_impl — OHOS audio VDI over the Android legacy audio HAL,
 * reached in-process through libhybris hw_get_module("audio.primary"), the
 * same way the display VDIs wrap hwcomposer and gralloc.
 *
 * Loaded by the VDI dispatcher (drivers/peripheral/audio/hdi_service/
 * primary_impl/vdi_src/audio_manager_vdi.c), which dlopens this library by
 * its fixed /vendor/lib64 path and dlsym's AudioManagerCreateIfInstance.
 * See device/board/oniro/docs/hybris_generic/audio_hal_switch_plan.md.
 */

#include "hybris_audio_common.h"
#include "hybris_audio_internal.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

#include "parameter.h"

namespace OHOS::HDI::Audio::Hybris {

// ─── Runtime libhardware loader ─────────────────────────────────────────────
// deps_guard's Passthrough rule forbids a link-time dep on libhardware. We
// dlopen it instead: the lib lives in /system/lib64/ at runtime (built by
// third_party/libhybris/hybris/hardware) and itself wraps the Android HAL
// loader via libhybris-common's HYBRIS_LIBRARY_INITIALIZE macro.
typedef int (*hw_get_module_by_class_fn)(const char *, const char *,
    const struct hw_module_t **);
typedef int (*hw_get_module_fn)(const char *, const struct hw_module_t **);

static void *g_libhw = nullptr;
static hw_get_module_by_class_fn g_hw_get_by_class = nullptr;
static hw_get_module_fn g_hw_get = nullptr;

static bool LoadHybrisHardware()
{
    if (g_libhw) return true;
    static const char *paths[] = {
        "libhardware.z.so",
        "/system/lib64/libhardware.z.so",
        "/system/lib/libhardware.z.so",
        "libhardware.so",
        nullptr,
    };
    for (int i = 0; paths[i]; ++i) {
        g_libhw = dlopen(paths[i], RTLD_LAZY);
        if (g_libhw) break;
    }
    if (!g_libhw) {
        HB_LOGE("dlopen(libhardware) failed: %{public}s", dlerror());
        return false;
    }
    g_hw_get_by_class = (hw_get_module_by_class_fn)dlsym(g_libhw, "hw_get_module_by_class");
    g_hw_get          = (hw_get_module_fn)dlsym(g_libhw, "hw_get_module");
    if (!g_hw_get_by_class || !g_hw_get) {
        HB_LOGE("dlsym(hw_get_module*) failed");
        return false;
    }
    return true;
}

int HybrisHwGetModuleByClass(const char *class_id, const char *inst,
    const struct hw_module_t **module)
{
    if (!LoadHybrisHardware()) return -ENOSYS;
    return g_hw_get_by_class(class_id, inst, module);
}

int HybrisHwGetModule(const char *id, const struct hw_module_t **module)
{
    if (!LoadHybrisHardware()) return -ENOSYS;
    return g_hw_get(id, module);
}

// ─── type conversion helpers ────────────────────────────────────────────────

audio_format_t VdiFormatToAndroid(enum AudioFormatVdi f)
{
    switch (f) {
        case AUDIO_VDI_FORMAT_TYPE_PCM_8_BIT:  return AUDIO_FORMAT_PCM_8_BIT;
        case AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT: return AUDIO_FORMAT_PCM_16_BIT;
        case AUDIO_VDI_FORMAT_TYPE_PCM_24_BIT: return AUDIO_FORMAT_PCM_24_BIT_PACKED;
        case AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT: return AUDIO_FORMAT_PCM_32_BIT;
        case AUDIO_VDI_FORMAT_TYPE_PCM_FLOAT:  return AUDIO_FORMAT_PCM_FLOAT;
        default: return AUDIO_FORMAT_PCM_16_BIT;
    }
}

enum AudioFormatVdi AndroidFormatToVdi(audio_format_t f)
{
    switch (f) {
        case AUDIO_FORMAT_PCM_8_BIT:         return AUDIO_VDI_FORMAT_TYPE_PCM_8_BIT;
        case AUDIO_FORMAT_PCM_16_BIT:        return AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT;
        case AUDIO_FORMAT_PCM_24_BIT_PACKED: return AUDIO_VDI_FORMAT_TYPE_PCM_24_BIT;
        case AUDIO_FORMAT_PCM_32_BIT:        return AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT;
        case AUDIO_FORMAT_PCM_FLOAT:         return AUDIO_VDI_FORMAT_TYPE_PCM_FLOAT;
        default:                             return AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT;
    }
}

audio_channel_mask_t VdiChannelsToAndroidOut(uint32_t channelCount)
{
    return (channelCount == 1) ? AUDIO_CHANNEL_OUT_MONO : AUDIO_CHANNEL_OUT_STEREO;
}

audio_channel_mask_t VdiChannelsToAndroidIn(uint32_t channelCount)
{
    return (channelCount == 1) ? AUDIO_CHANNEL_IN_MONO : AUDIO_CHANNEL_IN_STEREO;
}

/*
 * AudioPortPinVdi looks like a bitfield but is not one: every input pin
 * carries bit 27 (PIN_VDI_IN_MIC = 1<<27|1<<0, PIN_VDI_IN_HS_MIC = 1<<27|1<<1,
 * …) and PIN_VDI_OUT_EARPIECE is 1<<5|1<<4, overlapping the two USB pins.
 * Testing with & therefore matches several devices at once — asking the HAL
 * for the built-in mic used to open mic+headset+line-in+USB+SCO together and
 * yielded silence.  Match exact values.
 */
audio_devices_t VdiPinToAndroidOutDevice(enum AudioPortPinVdi pin)
{
    switch (pin) {
        case PIN_VDI_OUT_SPEAKER:         return AUDIO_DEVICE_OUT_SPEAKER;
        case PIN_VDI_OUT_HEADSET:         return AUDIO_DEVICE_OUT_WIRED_HEADSET;
        case PIN_VDI_OUT_HEADPHONE:       return AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
        case PIN_VDI_OUT_LINEOUT:         return AUDIO_DEVICE_OUT_LINE;
        case PIN_VDI_OUT_HDMI:            return AUDIO_DEVICE_OUT_HDMI;
        case PIN_VDI_OUT_EARPIECE:        return AUDIO_DEVICE_OUT_EARPIECE;
        case PIN_VDI_OUT_USB:
        case PIN_VDI_OUT_USB_EXT:         return AUDIO_DEVICE_OUT_USB_DEVICE;
        case PIN_VDI_OUT_USB_HEADSET:     return AUDIO_DEVICE_OUT_USB_HEADSET;
        case PIN_VDI_OUT_BLUETOOTH_SCO:   return AUDIO_DEVICE_OUT_BLUETOOTH_SCO;
        case PIN_VDI_OUT_BLUETOOTH_A2DP:  return AUDIO_DEVICE_OUT_BLUETOOTH_A2DP;
        default:                          return AUDIO_DEVICE_OUT_SPEAKER;
    }
}

audio_devices_t VdiPinToAndroidInDevice(enum AudioPortPinVdi pin)
{
    switch (pin) {
        case PIN_VDI_IN_MIC:                    return AUDIO_DEVICE_IN_BUILTIN_MIC;
        case PIN_VDI_IN_HS_MIC:                 return AUDIO_DEVICE_IN_WIRED_HEADSET;
        case PIN_VDI_IN_LINEIN:                 return AUDIO_DEVICE_IN_LINE;
        case PIN_VDI_IN_USB_EXT:
        case PIN_VDI_IN_USB_HEADSET:            return AUDIO_DEVICE_IN_USB_HEADSET;
        case PIN_VDI_IN_BLUETOOTH_SCO_HEADSET:  return AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
        default:                                return AUDIO_DEVICE_IN_BUILTIN_MIC;
    }
}

audio_source_t VdiInputTypeToAndroidSource(int32_t sourceType)
{
    if (sourceType & AUDIO_VDI_INPUT_VOICE_COMMUNICATION_TYPE) return AUDIO_SOURCE_VOICE_COMMUNICATION;
    if (sourceType & AUDIO_VDI_INPUT_VOICE_RECOGNITION_TYPE)   return AUDIO_SOURCE_VOICE_RECOGNITION;
    if (sourceType & AUDIO_VDI_INPUT_VOICE_CALL_TYPE)          return AUDIO_SOURCE_VOICE_CALL;
    if (sourceType & AUDIO_VDI_INPUT_CAMCORDER_TYPE)           return AUDIO_SOURCE_CAMCORDER;
    return AUDIO_SOURCE_MIC;
}

// ─── Singleton manager ──────────────────────────────────────────────────────

static HybrisAudioManager *g_manager = nullptr;
static std::mutex g_managerLock;

// ─── IAudioManagerVdi vtable ────────────────────────────────────────────────

static int32_t ManagerGetAllAdapters(struct IAudioManagerVdi *self,
    struct AudioAdapterDescriptorVdi *descs, uint32_t *descsLen)
{
    if (!self || !descs || !descsLen) return HDF_ERR_INVALID_PARAM;
    if (*descsLen < 1) { *descsLen = 1; return HDF_ERR_NOT_SUPPORT; }

    // One "primary" adapter with an OUT and an IN port.  The adapter name is
    // matched by audio_policy_config.xml; the port descriptions are opaque
    // labels and nothing consults them.
    static char primaryName[] = "primary";
    static struct AudioPortVdi primaryPorts[2] = {
        { PORT_VDI_OUT, 0, (char *)"AOP_primary_out" },
        { PORT_VDI_IN,  1, (char *)"AIP_primary_in"  },
    };
    descs[0].adapterName = primaryName;
    descs[0].ports = primaryPorts;
    descs[0].portsLen = 2;
    *descsLen = 1;
    HB_LOGI("GetAllAdapters: returning [primary]");
    return HDF_SUCCESS;
}

static int32_t ManagerLoadAdapter(struct IAudioManagerVdi *self,
    const struct AudioAdapterDescriptorVdi *desc, struct IAudioAdapterVdi **adapter)
{
    if (!self || !desc || !adapter) return HDF_ERR_INVALID_PARAM;
    auto *mgr = reinterpret_cast<HybrisAudioManager *>(self);
    std::lock_guard<std::mutex> lk(mgr->lock);

    if (!desc->adapterName || strcmp(desc->adapterName, "primary") != 0) {
        HB_LOGW("LoadAdapter: unsupported '%{public}s'", desc->adapterName ? desc->adapterName : "(null)");
        return HDF_ERR_NOT_SUPPORT;
    }

    if (mgr->adapter) {
        HB_LOGI("LoadAdapter: primary already loaded, reusing");
        *adapter = &mgr->adapter->vtable;
        return HDF_SUCCESS;
    }

    // The Android HAL reads the container's property store and binder-connects
    // to services it starts (mtkpower, IMtkAudio). Loading it before androidd
    // has the container up gets a half-initialised HAL that never recovers, so
    // wait for the same readiness signal the display VDIs use.
    if (WaitParameter(ANDROID_READY_PARAM, "1", ANDROID_READY_TIMEOUT_S) != 0) {
        HB_LOGE("timed out waiting for %{public}s — container not up", ANDROID_READY_PARAM);
        return HDF_FAILURE;
    }

    struct hw_module_t *hwmod = nullptr;
    int rc = HybrisHwGetModuleByClass(AUDIO_HARDWARE_MODULE_ID, "primary",
        const_cast<const hw_module_t **>(&hwmod));
    if (rc != 0 || !hwmod) {
        HB_LOGE("hw_get_module_by_class(audio.primary) failed rc=%{public}d", rc);
        return HDF_FAILURE;
    }

    struct audio_hw_device *hwdev = nullptr;
    rc = audio_hw_device_open(hwmod, &hwdev);
    if (rc != 0 || !hwdev) {
        HB_LOGE("audio_hw_device_open failed rc=%{public}d", rc);
        return HDF_FAILURE;
    }
    if (hwdev->init_check && hwdev->init_check(hwdev) != 0) {
        HB_LOGE("audio hwdev init_check failed");
        audio_hw_device_close(hwdev);
        return HDF_FAILURE;
    }

    auto *ad = new (std::nothrow) HybrisAudioAdapter();
    if (!ad) { audio_hw_device_close(hwdev); return HDF_ERR_MALLOC_FAIL; }
    ad->hwdev = hwdev;
    ad->adapterName = "primary";
    InitAdapterVTable(&ad->vtable);

    mgr->adapter = ad;
    *adapter = &ad->vtable;
    HB_LOGI("LoadAdapter: primary opened, Android HAL version=0x%{public}x", hwdev->common.version);
    return HDF_SUCCESS;
}

static int32_t ManagerUnloadAdapter(struct IAudioManagerVdi *self, struct IAudioAdapterVdi *adapter)
{
    if (!self || !adapter) return HDF_ERR_INVALID_PARAM;
    auto *mgr = reinterpret_cast<HybrisAudioManager *>(self);
    std::lock_guard<std::mutex> lk(mgr->lock);

    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(adapter);
    if (mgr->adapter != ad) {
        HB_LOGW("UnloadAdapter: unknown adapter");
        return HDF_ERR_INVALID_OBJECT;
    }
    StopJackWatcher(ad);
    if (ad->hwdev) {
        audio_hw_device_close(ad->hwdev);
        ad->hwdev = nullptr;
    }
    delete ad;
    mgr->adapter = nullptr;
    HB_LOGI("UnloadAdapter: primary released");
    return HDF_SUCCESS;
}

static int32_t ManagerReleaseObject(struct IAudioManagerVdi *self)
{
    if (!self) return HDF_ERR_INVALID_PARAM;
    auto *mgr = reinterpret_cast<HybrisAudioManager *>(self);
    std::lock_guard<std::mutex> gk(g_managerLock);
    if (mgr->adapter) {
        ManagerUnloadAdapter(self, &mgr->adapter->vtable);
    }
    if (mgr == g_manager) {
        g_manager = nullptr;
    }
    delete mgr;
    HB_LOGI("ReleaseAudioManagerObject");
    return HDF_SUCCESS;
}

} // namespace

// ─── Exported entry point — dlsym'd by VDI dispatcher ───────────────────────

using namespace OHOS::HDI::Audio::Hybris;

extern "C" struct IAudioManagerVdi *AudioManagerCreateIfInstance(void)
{
    std::lock_guard<std::mutex> gk(g_managerLock);
    if (g_manager) {
        return &g_manager->vtable;
    }
    auto *mgr = new (std::nothrow) HybrisAudioManager();
    if (!mgr) {
        HB_LOGE("OOM creating HybrisAudioManager");
        return nullptr;
    }
    mgr->vtable.GetAllAdapters             = ManagerGetAllAdapters;
    mgr->vtable.LoadAdapter                = ManagerLoadAdapter;
    mgr->vtable.UnloadAdapter              = ManagerUnloadAdapter;
    mgr->vtable.ReleaseAudioManagerObject  = ManagerReleaseObject;
    g_manager = mgr;
    HB_LOGI("AudioManagerCreateIfInstance: libhybris bridge ready");
    return &mgr->vtable;
}

extern "C" int32_t AudioManagerReleaseIfInstance(struct IAudioManagerVdi *mgr)
{
    if (!mgr) return HDF_ERR_INVALID_PARAM;
    return ManagerReleaseObject(mgr);
}
