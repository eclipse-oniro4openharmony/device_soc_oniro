/*
 * Copyright (c) 2026 Oniro Authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hybris_audio_common.h"
#include "hybris_audio_internal.h"

#include <cstdio>
#include <cstring>

namespace OHOS::HDI::Audio::Hybris {

#define C_SELF(self) auto *c = reinterpret_cast<HybrisAudioCapture *>(self)

static int32_t CaptureCaptureFrame(struct IAudioCaptureVdi *self,
    int8_t *frame, uint32_t *frameLen, uint64_t *replyBytes)
{
    if (!self || !frame || !frameLen || !replyBytes) return HDF_ERR_INVALID_PARAM;
    C_SELF(self);
    if (!c->stream) return HDF_FAILURE;
    std::lock_guard<std::mutex> lk(c->lock);

    // The framework asks for 8192-byte fragments while the HAL's own buffer is
    // one 20 ms period.  MTK's read() serves at most one period per call, so a
    // single read returns short and the caller — which treats the reply as the
    // whole fragment — ends up with a fraction of the audio it asked for.  Loop
    // until the request is satisfied.
    uint32_t want = *frameLen;
    uint32_t got = 0;
    while (got < want) {
        ssize_t n = c->stream->read(c->stream, frame + got, want - got);
        if (n < 0) {
            HB_LOGE("read failed after %{public}u/%{public}u bytes: %{public}zd", got, want, n);
            break;
        }
        if (n == 0) {
            HB_LOGW("read returned 0 after %{public}u/%{public}u bytes", got, want);
            break;
        }
        got += (uint32_t)n;
    }

    if (got == 0) {
        *replyBytes = 0;
        *frameLen = 0;
        return HDF_FAILURE;
    }
    *replyBytes = got;
    *frameLen = got;
    c->started.store(true);
    return HDF_SUCCESS;
}

static int32_t CaptureCaptureFrameEc(struct IAudioCaptureVdi *self, struct AudioCaptureFrameInfoVdi *info)
{
    if (!self || !info) return HDF_ERR_INVALID_PARAM;
    // Stub: dispatch only the primary frame; EC frame left empty.
    uint32_t len = info->frameLen;
    uint64_t reply = 0;
    int32_t rc = CaptureCaptureFrame(self, info->frame, &len, &reply);
    info->frameLen = len;
    info->replyBytes = reply;
    info->frameEcLen = 0;
    info->replyBytesEc = 0;
    return rc;
}

static int32_t CaptureGetCapturePosition(struct IAudioCaptureVdi *self,
    uint64_t *frames, struct AudioTimeStampVdi *time)
{
    C_SELF(self);
    if (!c->stream || !c->stream->get_capture_position) {
        if (frames) *frames = 0;
        if (time) { time->tvSec = 0; time->tvNSec = 0; }
        return HDF_SUCCESS;
    }
    int64_t fr = 0, ts = 0;
    int rc = c->stream->get_capture_position(c->stream, &fr, &ts);
    if (rc != 0) return HDF_FAILURE;
    if (frames) *frames = (uint64_t)fr;
    if (time) { time->tvSec = ts / 1000000000; time->tvNSec = ts % 1000000000; }
    return HDF_SUCCESS;
}

static int32_t CaptureCheckSceneCapability(struct IAudioCaptureVdi *,
    const struct AudioSceneDescriptorVdi *, bool *supported)
{ if (supported) *supported = true; return HDF_SUCCESS; }

static int32_t CaptureSelectScene(struct IAudioCaptureVdi *, const struct AudioSceneDescriptorVdi *)
{ return HDF_SUCCESS; }

static int32_t CaptureSetMute(struct IAudioCaptureVdi *self, bool mute)
{ C_SELF(self); c->muted = mute; return HDF_SUCCESS; }

static int32_t CaptureGetMute(struct IAudioCaptureVdi *self, bool *mute)
{ C_SELF(self); if (mute) *mute = c->muted; return HDF_SUCCESS; }

static int32_t CaptureSetVolume(struct IAudioCaptureVdi *self, float volume)
{
    C_SELF(self);
    c->volume = volume;
    if (c->stream && c->stream->set_gain) {
        return c->stream->set_gain(c->stream, volume) == 0 ? HDF_SUCCESS : HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int32_t CaptureGetVolume(struct IAudioCaptureVdi *self, float *volume)
{ C_SELF(self); if (volume) *volume = c->volume; return HDF_SUCCESS; }

static int32_t CaptureGetGainThreshold(struct IAudioCaptureVdi *, float *min, float *max)
{ if (min) *min = 0.0f; if (max) *max = 1.0f; return HDF_SUCCESS; }

static int32_t CaptureGetGain(struct IAudioCaptureVdi *self, float *gain)
{ C_SELF(self); if (gain) *gain = c->volume; return HDF_SUCCESS; }

static int32_t CaptureSetGain(struct IAudioCaptureVdi *self, float gain)
{ return CaptureSetVolume(self, gain); }

static int32_t CaptureGetFrameSize(struct IAudioCaptureVdi *self, uint64_t *size)
{
    C_SELF(self);
    if (!c->stream) { if (size) *size = 0; return HDF_SUCCESS; }
    if (size) *size = c->stream->common.get_buffer_size(&c->stream->common);
    return HDF_SUCCESS;
}

static int32_t CaptureGetFrameCount(struct IAudioCaptureVdi *self, uint64_t *count)
{
    C_SELF(self);
    if (!c->stream) { if (count) *count = 0; return HDF_SUCCESS; }
    size_t bs = c->stream->common.get_buffer_size(&c->stream->common);
    audio_format_t fmt = c->stream->common.get_format(&c->stream->common);
    audio_channel_mask_t cm = c->stream->common.get_channels(&c->stream->common);
    int frameSz = audio_bytes_per_sample(fmt) * popcount(cm);
    if (count) *count = (frameSz > 0) ? bs / frameSz : 0;
    return HDF_SUCCESS;
}

static int32_t CaptureSetSampleAttributes(struct IAudioCaptureVdi *self,
    const struct AudioSampleAttributesVdi *attrs)
{ C_SELF(self); if (attrs) c->attrs = *attrs; return HDF_SUCCESS; }

static int32_t CaptureGetSampleAttributes(struct IAudioCaptureVdi *self,
    struct AudioSampleAttributesVdi *attrs)
{ C_SELF(self); if (attrs) *attrs = c->attrs; return HDF_SUCCESS; }

static int32_t CaptureGetCurrentChannelId(struct IAudioCaptureVdi *self, uint32_t *channelId)
{ C_SELF(self); if (channelId) *channelId = c->attrs.channelCount; return HDF_SUCCESS; }

static int32_t CaptureSetExtraParams(struct IAudioCaptureVdi *self, const char *kv)
{
    C_SELF(self);
    if (!c->stream || !kv) return HDF_ERR_INVALID_PARAM;
    return c->stream->common.set_parameters(&c->stream->common, kv) == 0 ? HDF_SUCCESS : HDF_FAILURE;
}

static int32_t CaptureGetExtraParams(struct IAudioCaptureVdi *self, char *kv, uint32_t kvLen)
{
    C_SELF(self);
    if (!c->stream || !kv) return HDF_ERR_INVALID_PARAM;
    char *ret = c->stream->common.get_parameters(&c->stream->common, "");
    if (!ret) return HDF_FAILURE;
    strncpy(kv, ret, kvLen - 1);
    free(ret);
    return HDF_SUCCESS;
}

static int32_t CaptureReqMmapBuffer(struct IAudioCaptureVdi *, int32_t, struct AudioMmapBufferDescriptorVdi *)
{ return HDF_ERR_NOT_SUPPORT; }

static int32_t CaptureGetMmapPosition(struct IAudioCaptureVdi *,
    uint64_t *frames, struct AudioTimeStampVdi *time)
{ if (frames) *frames = 0; if (time) { time->tvSec = 0; time->tvNSec = 0; } return HDF_ERR_NOT_SUPPORT; }

static int32_t CaptureAddAudioEffect(struct IAudioCaptureVdi *, uint64_t) { return HDF_SUCCESS; }
static int32_t CaptureRemoveAudioEffect(struct IAudioCaptureVdi *, uint64_t) { return HDF_SUCCESS; }

static int32_t CaptureGetFrameBufferSize(struct IAudioCaptureVdi *self, uint64_t *bufferSize)
{ return CaptureGetFrameSize(self, bufferSize); }

static int32_t CaptureStart(struct IAudioCaptureVdi *self)
{
    C_SELF(self);
    if (!c->stream) return HDF_FAILURE;
    c->stream->common.set_parameters(&c->stream->common, "standby=0");
    c->started.store(true);
    return HDF_SUCCESS;
}

static int32_t CaptureStop(struct IAudioCaptureVdi *self)
{
    C_SELF(self);
    if (c->stream) c->stream->common.standby(&c->stream->common);
    c->started.store(false);
    return HDF_SUCCESS;
}

static int32_t CapturePause(struct IAudioCaptureVdi *self)  { return CaptureStop(self); }
static int32_t CaptureResume(struct IAudioCaptureVdi *self) { return CaptureStart(self); }
static int32_t CaptureFlush(struct IAudioCaptureVdi *)      { return HDF_SUCCESS; }

static int32_t CaptureTurnStandbyMode(struct IAudioCaptureVdi *self)
{
    C_SELF(self);
    if (c->stream) c->stream->common.standby(&c->stream->common);
    return HDF_SUCCESS;
}

static int32_t CaptureAudioDevDump(struct IAudioCaptureVdi *self, int32_t, int32_t fd)
{
    C_SELF(self);
    if (c->stream && c->stream->common.dump) c->stream->common.dump(&c->stream->common, fd);
    return HDF_SUCCESS;
}

static int32_t CaptureIsSupportsPauseAndResume(struct IAudioCaptureVdi *,
    bool *supportPause, bool *supportResume)
{
    if (supportPause)  *supportPause  = true;
    if (supportResume) *supportResume = true;
    return HDF_SUCCESS;
}

void InitCaptureVTable(struct IAudioCaptureVdi *v)
{
    v->CaptureFrame               = CaptureCaptureFrame;
    v->CaptureFrameEc             = CaptureCaptureFrameEc;
    v->GetCapturePosition         = CaptureGetCapturePosition;
    v->CheckSceneCapability       = CaptureCheckSceneCapability;
    v->SelectScene                = CaptureSelectScene;
    v->SetMute                    = CaptureSetMute;
    v->GetMute                    = CaptureGetMute;
    v->SetVolume                  = CaptureSetVolume;
    v->GetVolume                  = CaptureGetVolume;
    v->GetGainThreshold           = CaptureGetGainThreshold;
    v->GetGain                    = CaptureGetGain;
    v->SetGain                    = CaptureSetGain;
    v->GetFrameSize               = CaptureGetFrameSize;
    v->GetFrameCount              = CaptureGetFrameCount;
    v->SetSampleAttributes        = CaptureSetSampleAttributes;
    v->GetSampleAttributes        = CaptureGetSampleAttributes;
    v->GetCurrentChannelId        = CaptureGetCurrentChannelId;
    v->SetExtraParams             = CaptureSetExtraParams;
    v->GetExtraParams             = CaptureGetExtraParams;
    v->ReqMmapBuffer              = CaptureReqMmapBuffer;
    v->GetMmapPosition            = CaptureGetMmapPosition;
    v->AddAudioEffect             = CaptureAddAudioEffect;
    v->RemoveAudioEffect          = CaptureRemoveAudioEffect;
    v->GetFrameBufferSize         = CaptureGetFrameBufferSize;
    v->Start                      = CaptureStart;
    v->Stop                       = CaptureStop;
    v->Pause                      = CapturePause;
    v->Resume                     = CaptureResume;
    v->Flush                      = CaptureFlush;
    v->TurnStandbyMode            = CaptureTurnStandbyMode;
    v->AudioDevDump               = CaptureAudioDevDump;
    v->IsSupportsPauseAndResume   = CaptureIsSupportsPauseAndResume;
}

} // namespace
