/*
 * Copyright (c) 2026 Oniro Authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hybris_audio_common.h"
#include "hybris_audio_internal.h"

#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

namespace OHOS::HDI::Audio::Hybris {

extern void InitRenderVTable(struct IAudioRenderVdi *v);
extern void InitCaptureVTable(struct IAudioCaptureVdi *v);

// ─── Mixer poke ─────────────────────────────────────────────────────────────
// A raw ELEM_WRITE ioctl rather than alsa-lib: two integer writes do not
// justify pulling libasound into a driver host that otherwise never touches
// ALSA, and the uapi header alone is enough.

static bool WriteMixerInt(const char *name, int32_t value)
{
    struct snd_ctl_elem_value ev;
    int fd = open("/dev/snd/controlC0", O_RDWR);
    if (fd < 0) {
        return false;
    }

    memset(&ev, 0, sizeof(ev));
    ev.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy(reinterpret_cast<char *>(ev.id.name), name, sizeof(ev.id.name) - 1);

    // Absent controls fail the read; that is the "other SoC" case, not an error.
    bool ok = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) >= 0;
    if (ok) {
        ev.value.integer.value[0] = value;
        ok = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) >= 0;
    }
    close(fd);
    return ok;
}

// The MTK HAL routes capture through its DSP data provider whenever
// dsp_captureraw_default_en reads back non-zero, and hands the AFE an
// ADSP-shared buffer.  This SoC configuration has no ADSP core at all — the
// device tree carries scp@… but no adsp node and audio_ipi reports zero cores
// — so every frame then arrives empty.  Force the AP-side provider, which
// still runs the aurisys chain (where the uplink gain lives) and reads VUL9
// directly.  Controls are absent on SoCs without the DSP path, and the write
// is then a no-op.
static void ForceApCapturePath()
{
    static const char *ctrls[] = { "dsp_captureraw_default_en", "dsp_captureul1_default_en" };
    for (const char *c : ctrls) {
        if (WriteMixerInt(c, 0)) {
            HB_LOGI("capture: forced %{public}s=0 (AP provider)", c);
        }
    }
}

// ─── Render/Capture creation ────────────────────────────────────────────────

static int32_t AdapterInitAllPorts(struct IAudioAdapterVdi *self)
{
    (void)self;
    return HDF_SUCCESS;
}

static int32_t AdapterCreateRender(struct IAudioAdapterVdi *self,
    const struct AudioDeviceDescriptorVdi *desc, const struct AudioSampleAttributesVdi *attrs,
    struct IAudioRenderVdi **render)
{
    if (!self || !desc || !attrs || !render) return HDF_ERR_INVALID_PARAM;
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    std::lock_guard<std::mutex> lk(ad->lock);

    struct audio_config cfg = {};
    cfg.sample_rate = attrs->sampleRate ? attrs->sampleRate : 48000;
    cfg.channel_mask = VdiChannelsToAndroidOut(attrs->channelCount ? attrs->channelCount : 2);
    cfg.format = VdiFormatToAndroid(attrs->format);

    audio_devices_t devices = VdiPinToAndroidOutDevice(desc->pins);
    audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_PRIMARY;
    if (attrs->type == AUDIO_VDI_MMAP_NOIRQ || attrs->type == AUDIO_VDI_MMAP_VOIP) {
        flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_MMAP_NOIRQ);
    }

    struct audio_stream_out *stream = nullptr;
    int rc = ad->hwdev->open_output_stream(ad->hwdev, /*handle*/ 0, devices, flags, &cfg,
        &stream, /*address*/ "");
    if (rc != 0 || !stream) {
        HB_LOGE("open_output_stream failed rc=%{public}d devices=0x%{public}x", rc, devices);
        return HDF_FAILURE;
    }

    auto *r = new (std::nothrow) HybrisAudioRender();
    if (!r) { ad->hwdev->close_output_stream(ad->hwdev, stream); return HDF_ERR_MALLOC_FAIL; }
    r->stream = stream;
    r->owner = ad;
    r->attrs = *attrs;
    // HAL may mutate cfg; reflect that back
    r->attrs.sampleRate = cfg.sample_rate;
    r->attrs.format = AndroidFormatToVdi(cfg.format);
    r->attrs.channelCount = (cfg.channel_mask == AUDIO_CHANNEL_OUT_MONO) ? 1 : 2;
    r->desc = *desc;
    InitRenderVTable(&r->vtable);

    *render = &r->vtable;
    HB_LOGI("CreateRender: rate=%{public}u ch=%{public}u fmt=0x%{public}x dev=0x%{public}x flags=0x%{public}x",
        cfg.sample_rate, cfg.channel_mask, cfg.format, devices, flags);
    return HDF_SUCCESS;
}

static int32_t AdapterDestroyRender(struct IAudioAdapterVdi *self, struct IAudioRenderVdi *render)
{
    if (!self || !render) return HDF_ERR_INVALID_PARAM;
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    auto *r  = reinterpret_cast<HybrisAudioRender *>(render);
    std::lock_guard<std::mutex> lk(ad->lock);
    if (r->stream) {
        if (r->started) r->stream->common.standby(&r->stream->common);
        ad->hwdev->close_output_stream(ad->hwdev, r->stream);
        r->stream = nullptr;
    }
    delete r;
    HB_LOGI("DestroyRender");
    return HDF_SUCCESS;
}

static int32_t AdapterCreateCapture(struct IAudioAdapterVdi *self,
    const struct AudioDeviceDescriptorVdi *desc, const struct AudioSampleAttributesVdi *attrs,
    struct IAudioCaptureVdi **capture)
{
    if (!self || !desc || !attrs || !capture) return HDF_ERR_INVALID_PARAM;
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    std::lock_guard<std::mutex> lk(ad->lock);

    struct audio_config cfg = {};
    cfg.sample_rate = attrs->sampleRate ? attrs->sampleRate : 48000;
    cfg.channel_mask = VdiChannelsToAndroidIn(attrs->channelCount ? attrs->channelCount : 1);
    cfg.format = VdiFormatToAndroid(attrs->format);

    audio_devices_t devices = VdiPinToAndroidInDevice(desc->pins);
    audio_source_t source = VdiInputTypeToAndroidSource(attrs->sourceType);
    audio_input_flags_t flags = AUDIO_INPUT_FLAG_NONE;
    if (attrs->type == AUDIO_VDI_MMAP_NOIRQ || attrs->type == AUDIO_VDI_MMAP_VOIP) {
        flags = AUDIO_INPUT_FLAG_MMAP_NOIRQ;
    }

    ForceApCapturePath();

    struct audio_stream_in *stream = nullptr;
    int rc = ad->hwdev->open_input_stream(ad->hwdev, /*handle*/ 0, devices, &cfg, &stream,
        flags, /*address*/ "", source);
    if (rc != 0 || !stream) {
        HB_LOGE("open_input_stream failed rc=%{public}d devices=0x%{public}x source=%{public}d", rc, devices, source);
        return HDF_FAILURE;
    }

    auto *c = new (std::nothrow) HybrisAudioCapture();
    if (!c) { ad->hwdev->close_input_stream(ad->hwdev, stream); return HDF_ERR_MALLOC_FAIL; }
    c->stream = stream;
    c->owner  = ad;
    c->attrs  = *attrs;
    c->attrs.sampleRate   = cfg.sample_rate;
    c->attrs.format       = AndroidFormatToVdi(cfg.format);
    c->attrs.channelCount = (cfg.channel_mask == AUDIO_CHANNEL_IN_MONO) ? 1 : 2;
    c->desc = *desc;
    InitCaptureVTable(&c->vtable);

    *capture = &c->vtable;
    // Requested vs granted: the framework does not renegotiate, so any
    // difference here shows up downstream as wrong-speed or silent audio.
    HB_LOGI("CreateCapture: asked rate=%{public}u ch=%{public}u fmt=%{public}d | "
            "got rate=%{public}u mask=0x%{public}x fmt=0x%{public}x buf=%{public}zu | dev=0x%{public}x src=%{public}d",
        attrs->sampleRate, attrs->channelCount, (int)attrs->format,
        cfg.sample_rate, cfg.channel_mask, cfg.format,
        stream->common.get_buffer_size(&stream->common), devices, source);
    if (cfg.sample_rate != attrs->sampleRate || c->attrs.channelCount != attrs->channelCount) {
        HB_LOGE("CreateCapture: HAL refused the requested format — capture will be wrong-speed");
    }
    return HDF_SUCCESS;
}

static int32_t AdapterDestroyCapture(struct IAudioAdapterVdi *self, struct IAudioCaptureVdi *capture)
{
    if (!self || !capture) return HDF_ERR_INVALID_PARAM;
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    auto *c  = reinterpret_cast<HybrisAudioCapture *>(capture);
    std::lock_guard<std::mutex> lk(ad->lock);
    if (c->stream) {
        if (c->started) c->stream->common.standby(&c->stream->common);
        ad->hwdev->close_input_stream(ad->hwdev, c->stream);
        c->stream = nullptr;
    }
    delete c;
    HB_LOGI("DestroyCapture");
    return HDF_SUCCESS;
}

// ─── Capability / port ──────────────────────────────────────────────────────

static int32_t AdapterGetPortCapability(struct IAudioAdapterVdi *self,
    const struct AudioPortVdi *port, struct AudioPortCapabilityVdi *capability)
{
    if (!capability) return HDF_ERR_INVALID_PARAM;
    memset(capability, 0, sizeof(*capability));
    capability->deviceType = (port && port->dir == PORT_VDI_IN) ? AUDIO_VDI_PRIMARY_DEVICE : AUDIO_VDI_PRIMARY_DEVICE;
    capability->hardwareMode = true;
    capability->sampleRateMasks =
        AUDIO_VDI_SAMPLE_RATE_MASK_16000 | AUDIO_VDI_SAMPLE_RATE_MASK_44100 | AUDIO_VDI_SAMPLE_RATE_MASK_48000;
    capability->channelMasks = AUDIO_VDI_CHANNEL_STEREO;
    capability->channelCount = 2;
    return HDF_SUCCESS;
}

static int32_t AdapterSetPassthroughMode(struct IAudioAdapterVdi *, const struct AudioPortVdi *,
    enum AudioPortPassthroughModeVdi)
{ return HDF_SUCCESS; }

static int32_t AdapterGetPassthroughMode(struct IAudioAdapterVdi *, const struct AudioPortVdi *,
    enum AudioPortPassthroughModeVdi *mode)
{ if (mode) *mode = PORT_VDI_PASSTHROUGH_LPCM; return HDF_SUCCESS; }

static int32_t AdapterGetDeviceStatus(struct IAudioAdapterVdi *, struct AudioDeviceStatusVdi *status)
{ if (status) status->pnpStatus = 0; return HDF_SUCCESS; }

// ─── Routing / volume / mute ────────────────────────────────────────────────

static int32_t AdapterUpdateAudioRoute(struct IAudioAdapterVdi *self,
    const struct AudioRouteVdi *route, int32_t *routeHandle)
{
    if (!self || !route || !routeHandle) return HDF_ERR_INVALID_PARAM;
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    static std::atomic<int32_t> nextHandle{1};
    *routeHandle = nextHandle.fetch_add(1);

    audio_devices_t sinks = 0;
    for (uint32_t i = 0; i < route->sinksLen; ++i) {
        sinks |= VdiPinToAndroidOutDevice(route->sinks[i].ext.device.type);
    }
    if (sinks && ad->hwdev && ad->hwdev->set_parameters) {
        char kv[64];
        snprintf(kv, sizeof(kv), "routing=%u", (unsigned)sinks);
        ad->hwdev->set_parameters(ad->hwdev, kv);
        HB_LOGI("UpdateAudioRoute: %{public}s (handle=%{public}d)", kv, *routeHandle);
    }
    return HDF_SUCCESS;
}

static int32_t AdapterReleaseAudioRoute(struct IAudioAdapterVdi *, int32_t)
{ return HDF_SUCCESS; }

static int32_t AdapterSetMicMute(struct IAudioAdapterVdi *self, bool mute)
{
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    if (ad && ad->hwdev && ad->hwdev->set_mic_mute) {
        return ad->hwdev->set_mic_mute(ad->hwdev, mute) == 0 ? HDF_SUCCESS : HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t AdapterGetMicMute(struct IAudioAdapterVdi *self, bool *mute)
{
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    if (mute && ad && ad->hwdev && ad->hwdev->get_mic_mute) {
        return ad->hwdev->get_mic_mute(ad->hwdev, mute) == 0 ? HDF_SUCCESS : HDF_FAILURE;
    }
    if (mute) *mute = false;
    return HDF_SUCCESS;
}

static int32_t AdapterSetVoiceVolume(struct IAudioAdapterVdi *self, float volume)
{
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    if (ad && ad->hwdev && ad->hwdev->set_voice_volume) {
        return ad->hwdev->set_voice_volume(ad->hwdev, volume) == 0 ? HDF_SUCCESS : HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t AdapterSetExtraParams(struct IAudioAdapterVdi *self,
    enum AudioExtParamKeyVdi, const char *condition, const char *value)
{
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    if (!ad || !ad->hwdev || !ad->hwdev->set_parameters) return HDF_ERR_NOT_SUPPORT;
    char kv[256];
    snprintf(kv, sizeof(kv), "%s=%s", condition ? condition : "", value ? value : "");
    return ad->hwdev->set_parameters(ad->hwdev, kv) == 0 ? HDF_SUCCESS : HDF_FAILURE;
}

static int32_t AdapterGetExtraParams(struct IAudioAdapterVdi *self,
    enum AudioExtParamKeyVdi, const char *condition, char *value, uint32_t valueLen)
{
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    if (!ad || !ad->hwdev || !ad->hwdev->get_parameters) return HDF_ERR_NOT_SUPPORT;
    char *ret = ad->hwdev->get_parameters(ad->hwdev, condition ? condition : "");
    if (!ret) return HDF_FAILURE;
    if (value && valueLen) strncpy(value, ret, valueLen - 1);
    free(ret);
    return HDF_SUCCESS;
}

static int32_t AdapterRegExtraParamObserver(struct IAudioAdapterVdi *self,
    struct IAudioCallbackVdi *audioCallback, int8_t cookie)
{
    if (!self) return HDF_ERR_INVALID_PARAM;
    auto *ad = reinterpret_cast<HybrisAudioAdapter *>(self);
    std::lock_guard<std::mutex> lk(ad->lock);
    ad->paramObserver = audioCallback;
    ad->paramCookie = cookie;
    StartJackWatcher(ad);
    return HDF_SUCCESS;
}

static int32_t AdapterCreateCognitionStream(struct IAudioAdapterVdi *,
    const struct AudioSampleAttributesVdi *, int32_t *, struct AudioMmapBufferDescriptorVdi *)
{ return HDF_ERR_NOT_SUPPORT; }

static int32_t AdapterDestroyCognitionStream(struct IAudioAdapterVdi *, int32_t)
{ return HDF_ERR_NOT_SUPPORT; }

static int32_t AdapterNotifyCognitionData(struct IAudioAdapterVdi *, int32_t, uint32_t, uint32_t)
{ return HDF_ERR_NOT_SUPPORT; }

void InitAdapterVTable(struct IAudioAdapterVdi *v)
{
    v->InitAllPorts            = AdapterInitAllPorts;
    v->CreateRender            = AdapterCreateRender;
    v->DestroyRender           = AdapterDestroyRender;
    v->CreateCapture           = AdapterCreateCapture;
    v->DestroyCapture          = AdapterDestroyCapture;
    v->GetPortCapability       = AdapterGetPortCapability;
    v->SetPassthroughMode      = AdapterSetPassthroughMode;
    v->GetPassthroughMode      = AdapterGetPassthroughMode;
    v->GetDeviceStatus         = AdapterGetDeviceStatus;
    v->UpdateAudioRoute        = AdapterUpdateAudioRoute;
    v->ReleaseAudioRoute       = AdapterReleaseAudioRoute;
    v->SetMicMute              = AdapterSetMicMute;
    v->GetMicMute              = AdapterGetMicMute;
    v->SetVoiceVolume          = AdapterSetVoiceVolume;
    v->SetExtraParams          = AdapterSetExtraParams;
    v->GetExtraParams          = AdapterGetExtraParams;
    v->RegExtraParamObserver   = AdapterRegExtraParamObserver;
    v->CreateCognitionStream   = AdapterCreateCognitionStream;
    v->DestroyCognitionStream  = AdapterDestroyCognitionStream;
    v->NotifyCognitionData     = AdapterNotifyCognitionData;
}

// ─── Headset jack watcher (plan A5; untested until routing lands) ─────────

static int ReadJackState()
{
    static const char *paths[] = {
        "/sys/class/switch/h2w/state",
        "/sys/class/extcon/extcon0/state",
        nullptr,
    };
    for (int i = 0; paths[i]; ++i) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) continue;
        char buf[32] = {};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        // h2w: "0"=none, "1"=headset, "2"=headphone. extcon lines vary.
        return (int)strtol(buf, nullptr, 10);
    }
    return -1;
}

void StartJackWatcher(HybrisAudioAdapter *ad)
{
    if (!ad || ad->jackRun.load()) return;
    ad->jackRun.store(true);
    ad->jackThread = std::thread([ad]() {
        int prev = -1;
        while (ad->jackRun.load()) {
            int cur = ReadJackState();
            if (cur >= 0 && cur != prev) {
                auto *cb = ad->paramObserver;
                if (cb && cb->ParamCallback) {
                    const char *cond = "headset";
                    const char *val = (cur != 0) ? "1" : "0";
                    cb->ParamCallback(cb, AUDIO_VDI_EXT_PARAM_KEY_STATUS, cond, val,
                        nullptr, ad->paramCookie);
                    HB_LOGI("jack state %{public}d -> %{public}d (emitted)", prev, cur);
                }
                prev = cur;
            }
            // 500ms poll
            for (int i = 0; i < 5 && ad->jackRun.load(); ++i) {
                usleep(100 * 1000);
            }
        }
    });
}

void StopJackWatcher(HybrisAudioAdapter *ad)
{
    if (!ad) return;
    ad->jackRun.store(false);
    if (ad->jackThread.joinable()) ad->jackThread.join();
}

} // namespace
