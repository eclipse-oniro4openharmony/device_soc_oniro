/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hybris_audio_common.h"
#include "hybris_audio_internal.h"

#include <cstdio>
#include <cstring>

namespace OHOS::HDI::Audio::Hybris {

#define R_SELF(self) auto *r = reinterpret_cast<HybrisAudioRender *>(self)

static int32_t RenderGetLatency(struct IAudioRenderVdi *self, uint32_t *ms)
{
    if (!self || !ms) return HDF_ERR_INVALID_PARAM;
    R_SELF(self);
    if (!r->stream || !r->stream->get_latency) { *ms = 0; return HDF_SUCCESS; }
    *ms = r->stream->get_latency(r->stream);
    return HDF_SUCCESS;
}

static int32_t RenderRenderFrame(struct IAudioRenderVdi *self,
    const int8_t *frame, uint32_t frameLen, uint64_t *replyBytes)
{
    if (!self || !frame || !replyBytes) return HDF_ERR_INVALID_PARAM;
    R_SELF(self);
    if (!r->stream) return HDF_FAILURE;
    std::lock_guard<std::mutex> lk(r->lock);
    ssize_t n = r->stream->write(r->stream, frame, frameLen);
    if (n < 0) {
        HB_LOGE("stream_out->write failed n=%{public}zd", n);
        *replyBytes = 0;
        return HDF_FAILURE;
    }
    *replyBytes = (uint64_t)n;
    if (!r->started.load()) r->started.store(true);
    return HDF_SUCCESS;
}

static int32_t RenderGetRenderPosition(struct IAudioRenderVdi *self,
    uint64_t *frames, struct AudioTimeStampVdi *time)
{
    if (!self || !frames || !time) return HDF_ERR_INVALID_PARAM;
    R_SELF(self);
    if (!r->stream || !r->stream->get_presentation_position) {
        *frames = 0;
        time->tvSec = 0; time->tvNSec = 0;
        return HDF_SUCCESS;
    }
    struct timespec ts = {};
    uint64_t fr = 0;
    int rc = r->stream->get_presentation_position(r->stream, &fr, &ts);
    if (rc != 0) return HDF_FAILURE;
    *frames = fr;
    time->tvSec = ts.tv_sec;
    time->tvNSec = ts.tv_nsec;
    return HDF_SUCCESS;
}

static int32_t RenderSetRenderSpeed(struct IAudioRenderVdi *, float) { return HDF_ERR_NOT_SUPPORT; }
static int32_t RenderGetRenderSpeed(struct IAudioRenderVdi *, float *sp) { if (sp) *sp = 1.0f; return HDF_SUCCESS; }
static int32_t RenderSetChannelMode(struct IAudioRenderVdi *, enum AudioChannelModeVdi) { return HDF_SUCCESS; }
static int32_t RenderGetChannelMode(struct IAudioRenderVdi *, enum AudioChannelModeVdi *m)
{ if (m) *m = AUDIO_VDI_CHANNEL_NORMAL; return HDF_SUCCESS; }

static int32_t RenderRegCallback(struct IAudioRenderVdi *, RenderCallbackVdi, void *)
{ return HDF_SUCCESS; }

static int32_t RenderDrainBuffer(struct IAudioRenderVdi *self, enum AudioDrainNotifyTypeVdi *type)
{
    R_SELF(self);
    if (r->stream && r->stream->drain) {
        audio_drain_type_t t = (type && *type == AUDIO_VDI_DRAIN_EARLY_MODE)
            ? AUDIO_DRAIN_EARLY_NOTIFY : AUDIO_DRAIN_ALL;
        r->stream->drain(r->stream, t);
    }
    return HDF_SUCCESS;
}

static int32_t RenderIsSupportsDrain(struct IAudioRenderVdi *self, bool *support)
{
    R_SELF(self);
    if (support) *support = r->stream && r->stream->drain != nullptr;
    return HDF_SUCCESS;
}

static int32_t RenderCheckSceneCapability(struct IAudioRenderVdi *,
    const struct AudioSceneDescriptorVdi *, bool *supported)
{ if (supported) *supported = true; return HDF_SUCCESS; }

static int32_t RenderSelectScene(struct IAudioRenderVdi *self,
    const struct AudioSceneDescriptorVdi *scene)
{
    if (!self || !scene) return HDF_ERR_INVALID_PARAM;
    R_SELF(self);
    ApplyOutputScene(r->owner, scene);
    return HDF_SUCCESS;
}

static int32_t RenderSetMute(struct IAudioRenderVdi *self, bool mute)
{
    R_SELF(self);
    r->muted = mute;
    if (r->stream && r->stream->set_volume) {
        r->stream->set_volume(r->stream, mute ? 0.0f : r->volume, mute ? 0.0f : r->volume);
    }
    return HDF_SUCCESS;
}

static int32_t RenderGetMute(struct IAudioRenderVdi *self, bool *mute)
{ R_SELF(self); if (mute) *mute = r->muted; return HDF_SUCCESS; }

static int32_t RenderSetVolume(struct IAudioRenderVdi *self, float volume)
{
    R_SELF(self);
    r->volume = volume;
    if (!r->muted && r->stream && r->stream->set_volume) {
        return r->stream->set_volume(r->stream, volume, volume) == 0 ? HDF_SUCCESS : HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t RenderSetVolumeWithRamp(struct IAudioRenderVdi *self, float volume, uint32_t)
{ return RenderSetVolume(self, volume); }

static int32_t RenderGetVolume(struct IAudioRenderVdi *self, float *volume)
{ R_SELF(self); if (volume) *volume = r->volume; return HDF_SUCCESS; }

static int32_t RenderGetGainThreshold(struct IAudioRenderVdi *, float *min, float *max)
{ if (min) *min = 0.0f; if (max) *max = 1.0f; return HDF_SUCCESS; }

static int32_t RenderGetGain(struct IAudioRenderVdi *self, float *gain)
{ R_SELF(self); if (gain) *gain = r->volume; return HDF_SUCCESS; }

static int32_t RenderSetGain(struct IAudioRenderVdi *self, float gain)
{ return RenderSetVolume(self, gain); }

static int32_t RenderGetFrameSize(struct IAudioRenderVdi *self, uint64_t *size)
{
    R_SELF(self);
    if (!r->stream) { if (size) *size = 0; return HDF_SUCCESS; }
    if (size) *size = r->stream->common.get_buffer_size(&r->stream->common);
    return HDF_SUCCESS;
}

static int32_t RenderGetFrameCount(struct IAudioRenderVdi *self, uint64_t *count)
{
    R_SELF(self);
    if (!r->stream) { if (count) *count = 0; return HDF_SUCCESS; }
    size_t bs = r->stream->common.get_buffer_size(&r->stream->common);
    audio_format_t fmt = r->stream->common.get_format(&r->stream->common);
    audio_channel_mask_t cm = r->stream->common.get_channels(&r->stream->common);
    int frameSz = audio_bytes_per_sample(fmt) * popcount(cm);
    if (count) *count = (frameSz > 0) ? bs / frameSz : 0;
    return HDF_SUCCESS;
}

static int32_t RenderSetSampleAttributes(struct IAudioRenderVdi *self,
    const struct AudioSampleAttributesVdi *attrs)
{
    R_SELF(self);
    if (attrs) r->attrs = *attrs;
    return HDF_SUCCESS;
}

static int32_t RenderGetSampleAttributes(struct IAudioRenderVdi *self, struct AudioSampleAttributesVdi *attrs)
{ R_SELF(self); if (attrs) *attrs = r->attrs; return HDF_SUCCESS; }

static int32_t RenderGetCurrentChannelId(struct IAudioRenderVdi *self, uint32_t *channelId)
{ R_SELF(self); if (channelId) *channelId = r->attrs.channelCount; return HDF_SUCCESS; }

static int32_t RenderSetExtraParams(struct IAudioRenderVdi *self, const char *kv)
{
    R_SELF(self);
    if (!r->stream || !kv) return HDF_ERR_INVALID_PARAM;
    return r->stream->common.set_parameters(&r->stream->common, kv) == 0 ? HDF_SUCCESS : HDF_FAILURE;
}

static int32_t RenderGetExtraParams(struct IAudioRenderVdi *self, char *kv, uint32_t kvLen)
{
    R_SELF(self);
    if (!r->stream || !kv) return HDF_ERR_INVALID_PARAM;
    char *ret = r->stream->common.get_parameters(&r->stream->common, "");
    if (!ret) return HDF_FAILURE;
    strncpy(kv, ret, kvLen - 1);
    free(ret);
    return HDF_SUCCESS;
}

static int32_t RenderReqMmapBuffer(struct IAudioRenderVdi *, int32_t, struct AudioMmapBufferDescriptorVdi *)
{ return HDF_ERR_NOT_SUPPORT; /* mmap fast path: plan A7 */ }

static int32_t RenderGetMmapPosition(struct IAudioRenderVdi *, uint64_t *frames, struct AudioTimeStampVdi *time)
{ if (frames) *frames = 0; if (time) { time->tvSec = 0; time->tvNSec = 0; } return HDF_ERR_NOT_SUPPORT; }

static int32_t RenderAddAudioEffect(struct IAudioRenderVdi *, uint64_t) { return HDF_SUCCESS; }
static int32_t RenderRemoveAudioEffect(struct IAudioRenderVdi *, uint64_t) { return HDF_SUCCESS; }

static int32_t RenderGetFrameBufferSize(struct IAudioRenderVdi *self, uint64_t *bufferSize)
{ return RenderGetFrameSize(self, bufferSize); }

static int32_t RenderStart(struct IAudioRenderVdi *self)
{
    R_SELF(self);
    if (!r->stream) return HDF_FAILURE;
    // Legacy HAL has implicit start-on-write; ensure out of standby.
    r->stream->common.set_parameters(&r->stream->common, "standby=0");
    r->started.store(true);
    r->paused.store(false);
    return HDF_SUCCESS;
}

static int32_t RenderStop(struct IAudioRenderVdi *self)
{
    R_SELF(self);
    if (r->stream && r->stream->common.standby) {
        r->stream->common.standby(&r->stream->common);
    }
    r->started.store(false);
    return HDF_SUCCESS;
}

static int32_t RenderPause(struct IAudioRenderVdi *self)
{
    R_SELF(self);
    if (r->stream && r->stream->pause) r->stream->pause(r->stream);
    else if (r->stream) r->stream->common.standby(&r->stream->common);
    r->paused.store(true);
    return HDF_SUCCESS;
}

static int32_t RenderResume(struct IAudioRenderVdi *self)
{
    R_SELF(self);
    if (r->stream && r->stream->resume) r->stream->resume(r->stream);
    r->paused.store(false);
    return HDF_SUCCESS;
}

static int32_t RenderFlush(struct IAudioRenderVdi *self)
{
    R_SELF(self);
    if (r->stream && r->stream->flush) r->stream->flush(r->stream);
    return HDF_SUCCESS;
}

static int32_t RenderTurnStandbyMode(struct IAudioRenderVdi *self)
{
    R_SELF(self);
    if (r->stream && r->stream->common.standby) {
        r->stream->common.standby(&r->stream->common);
    }
    return HDF_SUCCESS;
}

static int32_t RenderAudioDevDump(struct IAudioRenderVdi *self, int32_t, int32_t fd)
{
    R_SELF(self);
    if (r->stream && r->stream->common.dump) r->stream->common.dump(&r->stream->common, fd);
    return HDF_SUCCESS;
}

static int32_t RenderIsSupportsPauseAndResume(struct IAudioRenderVdi *self, bool *supportPause, bool *supportResume)
{
    R_SELF(self);
    if (supportPause)  *supportPause  = r->stream && r->stream->pause != nullptr;
    if (supportResume) *supportResume = r->stream && r->stream->resume != nullptr;
    return HDF_SUCCESS;
}

static int32_t RenderSetBufferSize(struct IAudioRenderVdi *, uint32_t) { return HDF_SUCCESS; }

void InitRenderVTable(struct IAudioRenderVdi *v)
{
    v->GetLatency             = RenderGetLatency;
    v->RenderFrame            = RenderRenderFrame;
    v->GetRenderPosition      = RenderGetRenderPosition;
    v->SetRenderSpeed         = RenderSetRenderSpeed;
    v->GetRenderSpeed         = RenderGetRenderSpeed;
    v->SetChannelMode         = RenderSetChannelMode;
    v->GetChannelMode         = RenderGetChannelMode;
    v->RegCallback            = RenderRegCallback;
    v->DrainBuffer            = RenderDrainBuffer;
    v->IsSupportsDrain        = RenderIsSupportsDrain;
    v->CheckSceneCapability   = RenderCheckSceneCapability;
    v->SelectScene            = RenderSelectScene;
    v->SetMute                = RenderSetMute;
    v->GetMute                = RenderGetMute;
    v->SetVolume              = RenderSetVolume;
    v->SetVolumeWithRamp      = RenderSetVolumeWithRamp;
    v->GetVolume              = RenderGetVolume;
    v->GetGainThreshold       = RenderGetGainThreshold;
    v->GetGain                = RenderGetGain;
    v->SetGain                = RenderSetGain;
    v->GetFrameSize           = RenderGetFrameSize;
    v->GetFrameCount          = RenderGetFrameCount;
    v->SetSampleAttributes    = RenderSetSampleAttributes;
    v->GetSampleAttributes    = RenderGetSampleAttributes;
    v->GetCurrentChannelId    = RenderGetCurrentChannelId;
    v->SetExtraParams         = RenderSetExtraParams;
    v->GetExtraParams         = RenderGetExtraParams;
    v->ReqMmapBuffer          = RenderReqMmapBuffer;
    v->GetMmapPosition        = RenderGetMmapPosition;
    v->AddAudioEffect         = RenderAddAudioEffect;
    v->RemoveAudioEffect      = RenderRemoveAudioEffect;
    v->GetFrameBufferSize     = RenderGetFrameBufferSize;
    v->Start                  = RenderStart;
    v->Stop                   = RenderStop;
    v->Pause                  = RenderPause;
    v->Resume                 = RenderResume;
    v->Flush                  = RenderFlush;
    v->TurnStandbyMode        = RenderTurnStandbyMode;
    v->AudioDevDump           = RenderAudioDevDump;
    v->IsSupportsPauseAndResume = RenderIsSupportsPauseAndResume;
    v->SetBufferSize          = RenderSetBufferSize;
}

} // namespace
