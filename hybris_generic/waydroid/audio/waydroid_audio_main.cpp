/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * waydroid_audio — a PulseAudio server for the Android container.
 *
 * Why this exists.  The container's audio HAL (audio.primary.waydroid) is
 * an ALSA client whose "default" PCM is alsa-lib's pulse plugin, so it
 * needs a PulseAudio server on the other end of a UNIX socket.  Upstream
 * Waydroid borrows the desktop's PulseAudio; OHOS has none — 6.1 replaced
 * the pulseaudio-hosted audio_server with the HPAE engine, and
 * //third_party/pulseaudio is now only the client library.  So we speak
 * the protocol ourselves.
 *
 * Without it the HAL's snd_pcm_open never succeeds.  The visible damage is
 * much worse than silence: AudioFlinger's mixer only advances one HAL
 * buffer per failed retry, roughly one percent of real time, so every
 * app that opens an audio track has its buffer back up and its media
 * clock crawl.  That is why videos in Instagram sat frozen on their first
 * frame — they were waiting on an audio clock that had all but stopped.
 *
 * Shape.  One thread runs an epoll loop over the listening socket and its
 * clients, parsing PulseAudio packets; each playback stream owns an
 * OhosSink whose render thread drains it into an OHOS AudioRenderer at
 * wall-clock speed (ohos_sink.h).  The render threads hand pacing credit
 * back through an eventfd, and the loop turns that credit into PA REQUEST
 * commands — which is the whole flow-control contract.
 *
 * Scope: playback only.  Microphone capture into the container is a
 * separate feature that upstream Waydroid does not have either; a
 * CREATE_RECORD_STREAM is refused cleanly, exactly as it is today.
 */

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "hilog/log.h"
#include "ohos_sink.h"
#include "pa_wire.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002500
#define LOG_TAG "waydroid_audio"

namespace waydroid {
namespace {

using namespace pa;

/* Matches waydroid.pulse_runtime_path in data/waydroid.prop: the host dir
 * is bind-mounted at the container's /run/xdg (waydroidd.c). */
constexpr const char *DEFAULT_SOCKET = "/data/waydroid/run/xdg/pulse/native";

constexpr const char *SINK_NAME = "waydroid_output";
constexpr const char *SOURCE_NAME = "waydroid_input";

/* Defaults for a client that leaves a buffer attribute unset (-1). */
constexpr uint64_t DEFAULT_MAXLENGTH_USEC = 2000000;
constexpr uint64_t DEFAULT_TLENGTH_USEC = 200000;
constexpr size_t ABSOLUTE_MAX_BUFFER = 16u * 1024 * 1024;

volatile sig_atomic_t g_stop = 0;

void OnSignal(int sig)
{
    (void)sig;
    g_stop = 1;
}

uint64_t NowUsec()
{
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL + static_cast<uint64_t>(tv.tv_usec);
}

struct Stream {
    uint32_t channel = 0;
    uint32_t index = 0;
    SampleSpec ss;
    ChannelMap cm;
    BufferAttr attr;
    bool corked = false;
    std::unique_ptr<OhosSink> sink;

    /* PulseAudio's write index: everything the client has handed us. */
    uint64_t writeIndex = 0;
    /* Bytes we have asked for with REQUEST and not yet been given. */
    uint64_t outstanding = 0;
    uint64_t underrunFor = 0;
    uint64_t playingFor = 0;
    uint64_t startedAtUsec = 0;

    bool drainPending = false;
    uint32_t drainTag = 0;
};

struct Connection {
    int fd = -1;
    uint32_t version = PROTOCOL_VERSION;
    bool authorized = false;
    std::vector<uint8_t> in;
    std::vector<uint8_t> out;
    std::map<uint32_t, std::unique_ptr<Stream>> streams;
    uint32_t nextChannel = 0;
};

class Server {
public:
    explicit Server(std::string socketPath) : socketPath_(std::move(socketPath)) {}
    ~Server() { Shutdown(); }

    bool Start();
    void Run();

private:
    /* wire helpers */
    void Send(Connection &c, uint32_t channel, const uint8_t *payload, size_t len, uint32_t flags);
    void SendTag(Connection &c, const TagWriter &t) { Send(c, CHANNEL_COMMAND, t.Data().data(), t.Size(), 0); }
    void SendReplyHeader(TagWriter &t, uint32_t tag);
    void SendSimpleAck(Connection &c, uint32_t tag);
    void SendError(Connection &c, uint32_t tag, uint32_t error);
    void SendEvent(Connection &c, uint32_t command, uint32_t streamChannel, const int64_t *extra);

    /* dispatch */
    bool HandlePacket(Connection &c, uint32_t channel, uint32_t flags, uint64_t offset,
                      const uint8_t *data, size_t len);
    bool HandleCommand(Connection &c, uint32_t command, uint32_t tag, TagReader &r);
    bool HandleCreatePlayback(Connection &c, uint32_t tag, TagReader &r);
    void HandleStreamData(Connection &c, uint32_t channel, uint32_t flags, const uint8_t *data, size_t len);

    Stream *FindStream(Connection &c, uint32_t channel);
    void MaybeRequest(Connection &c, Stream &s);
    void PumpSinks();

    /* plumbing */
    void Accept();
    bool ReadFrom(Connection &c);
    bool FlushOut(Connection &c);
    void Drop(int fd);
    void Shutdown();
    void UpdateEvents(Connection &c);

    std::string socketPath_;
    int listenFd_ = -1;
    int epollFd_ = -1;
    int eventFd_ = -1;
    std::map<int, std::unique_ptr<Connection>> conns_;
    uint32_t nextStreamIndex_ = 0;
    uint32_t nextClientIndex_ = 0;
};

void Server::SendReplyHeader(TagWriter &t, uint32_t tag)
{
    t.PutU32(COMMAND_REPLY);
    t.PutU32(tag);
}

void Server::SendSimpleAck(Connection &c, uint32_t tag)
{
    TagWriter t;
    SendReplyHeader(t, tag);
    SendTag(c, t);
}

void Server::SendError(Connection &c, uint32_t tag, uint32_t error)
{
    TagWriter t;
    t.PutU32(COMMAND_ERROR);
    t.PutU32(tag);
    t.PutU32(error);
    SendTag(c, t);
}

/* Server-to-client notifications carry tag -1; see protocol-native.c. */
void Server::SendEvent(Connection &c, uint32_t command, uint32_t streamChannel, const int64_t *extra)
{
    TagWriter t;
    t.PutU32(command);
    t.PutU32(INVALID_INDEX);
    t.PutU32(streamChannel);
    if (extra != nullptr) {
        t.PutS64(*extra);
    }
    SendTag(c, t);
}

void Server::Send(Connection &c, uint32_t channel, const uint8_t *payload, size_t len, uint32_t flags)
{
    uint8_t desc[DESCRIPTOR_SIZE];
    auto put = [&desc](size_t i, uint32_t v) {
        desc[i] = static_cast<uint8_t>(v >> 24);
        desc[i + 1] = static_cast<uint8_t>(v >> 16);
        desc[i + 2] = static_cast<uint8_t>(v >> 8);
        desc[i + 3] = static_cast<uint8_t>(v);
    };
    put(0, static_cast<uint32_t>(len));
    put(4, channel);
    put(8, 0);
    put(12, 0);
    put(16, flags);
    c.out.insert(c.out.end(), desc, desc + DESCRIPTOR_SIZE);
    c.out.insert(c.out.end(), payload, payload + len);
}

Stream *Server::FindStream(Connection &c, uint32_t channel)
{
    auto it = c.streams.find(channel);
    return it == c.streams.end() ? nullptr : it->second.get();
}

/*
 * PulseAudio's flow control: the client may only write what the server has
 * asked for.  We aim to keep tlength bytes buffered, so whenever the gap
 * between that target and (buffered + already requested) grows past minreq,
 * we ask for the difference.
 */
void Server::MaybeRequest(Connection &c, Stream &s)
{
    if (s.sink == nullptr) {
        return;
    }
    int64_t buffered = static_cast<int64_t>(s.sink->Buffered());
    int64_t owed = static_cast<int64_t>(s.attr.tlength) - buffered - static_cast<int64_t>(s.outstanding);
    if (owed < static_cast<int64_t>(s.attr.minreq)) {
        return;
    }
    s.outstanding += static_cast<uint64_t>(owed);
    TagWriter t;
    t.PutU32(COMMAND_REQUEST);
    t.PutU32(INVALID_INDEX);
    t.PutU32(s.channel);
    t.PutU32(static_cast<uint32_t>(owed));
    SendTag(c, t);
}

bool Server::HandleCreatePlayback(Connection &c, uint32_t tag, TagReader &r)
{
    auto s = std::make_unique<Stream>();
    std::string device;
    bool deviceNull = false;
    uint32_t sinkIndex = 0;
    uint32_t maxlength = 0;
    bool corked = false;
    uint32_t syncId = 0;
    uint8_t volChannels = 0;
    uint32_t volume = VOLUME_NORM;
    std::map<std::string, std::string> props;

    /* The v13 request layout, straight from pa_create_stream() in
     * src/pulse/stream.c.  Order matters more than the values do. */
    if (!r.GetSampleSpec(s->ss) || !r.GetChannelMap(s->cm) || !r.GetU32(sinkIndex) ||
        !r.GetString(device, &deviceNull) || !r.GetU32(maxlength) || !r.GetBool(corked) ||
        !r.GetU32(s->attr.tlength) || !r.GetU32(s->attr.prebuf) || !r.GetU32(s->attr.minreq) ||
        !r.GetU32(syncId) || !r.GetCVolume(volChannels, volume)) {
        HILOG_ERROR(LOG_CORE, "malformed CREATE_PLAYBACK_STREAM");
        return false;
    }
    s->attr.maxlength = maxlength;

    /* v12 flag block, then the v13 additions. */
    bool flag = false;
    for (int i = 0; i < 7; i++) {
        if (!r.GetBool(flag)) {
            HILOG_ERROR(LOG_CORE, "malformed CREATE_PLAYBACK_STREAM flags");
            return false;
        }
    }
    bool startMuted = false;
    bool adjustLatency = false;
    if (!r.GetBool(startMuted) || !r.GetBool(adjustLatency) || !r.GetProplist(props)) {
        HILOG_ERROR(LOG_CORE, "malformed CREATE_PLAYBACK_STREAM v13 tail");
        return false;
    }

    if (!s->ss.Valid()) {
        HILOG_ERROR(LOG_CORE, "invalid sample spec: fmt %{public}u ch %{public}u rate %{public}u",
                    s->ss.format, s->ss.channels, s->ss.rate);
        SendError(c, tag, ERR_INVALID);
        return true;
    }

    /* Fill in anything the client left to us, then make the numbers sane. */
    size_t frame = s->ss.FrameSize();
    if (s->attr.maxlength == INVALID_INDEX || s->attr.maxlength == 0) {
        s->attr.maxlength = static_cast<uint32_t>(s->ss.UsecToBytes(DEFAULT_MAXLENGTH_USEC));
    }
    s->attr.maxlength = static_cast<uint32_t>(
        std::min<size_t>(std::max<size_t>(s->attr.maxlength, frame), ABSOLUTE_MAX_BUFFER));
    if (s->attr.tlength == INVALID_INDEX || s->attr.tlength == 0) {
        s->attr.tlength = static_cast<uint32_t>(s->ss.UsecToBytes(DEFAULT_TLENGTH_USEC));
    }
    s->attr.tlength = static_cast<uint32_t>(
        std::min<size_t>(std::max<size_t>(s->attr.tlength, frame), s->attr.maxlength));
    if (s->attr.minreq == INVALID_INDEX || s->attr.minreq == 0) {
        s->attr.minreq = s->attr.tlength / 4;
    }
    s->attr.minreq = static_cast<uint32_t>(
        std::min<size_t>(std::max<size_t>(s->attr.minreq, frame), s->attr.tlength));
    if (s->attr.prebuf == INVALID_INDEX) {
        s->attr.prebuf = s->attr.tlength;
    }
    s->attr.prebuf = std::min(s->attr.prebuf, s->attr.tlength);

    s->sink = std::make_unique<OhosSink>();
    int efd = eventFd_;
    if (!s->sink->Open(s->ss, s->attr.maxlength, [efd] {
            uint64_t one = 1;
            ssize_t ignored = write(efd, &one, sizeof(one));
            (void)ignored;
        })) {
        SendError(c, tag, ERR_INVALID);
        return true;
    }
    s->corked = corked;
    s->sink->SetCorked(corked);

    s->channel = c.nextChannel++;
    s->index = nextStreamIndex_++;
    s->startedAtUsec = NowUsec();

    auto nameIt = props.find("media.name");
    HILOG_INFO(LOG_CORE,
               "playback stream %{public}u: %{public}u Hz %{public}u ch fmt %{public}u, "
               "tlength %{public}u minreq %{public}u maxlength %{public}u, corked %{public}d (%{public}s)",
               s->channel, s->ss.rate, s->ss.channels, s->ss.format, s->attr.tlength, s->attr.minreq,
               s->attr.maxlength, static_cast<int>(corked),
               nameIt == props.end() ? "unnamed" : nameIt->second.c_str());

    /* The reply libpulse expects for v13 (pa_create_stream_callback). */
    TagWriter t;
    SendReplyHeader(t, tag);
    t.PutU32(s->channel);
    t.PutU32(s->index);
    t.PutU32(s->attr.tlength); /* initial credit: the buffer is empty */
    t.PutU32(s->attr.maxlength);
    t.PutU32(s->attr.tlength);
    t.PutU32(s->attr.prebuf);
    t.PutU32(s->attr.minreq);
    t.PutSampleSpec(s->ss);
    t.PutChannelMap(s->cm);
    t.PutU32(0); /* sink index */
    t.PutString(SINK_NAME);
    t.PutBool(false); /* not suspended */
    t.PutUsec(s->sink->SinkLatencyUsec());
    SendTag(c, t);

    s->outstanding = s->attr.tlength;
    uint32_t channel = s->channel;
    c.streams[channel] = std::move(s);
    return true;
}

void Server::HandleStreamData(Connection &c, uint32_t channel, uint32_t flags, const uint8_t *data, size_t len)
{
    Stream *s = FindStream(c, channel);
    if (s == nullptr || s->sink == nullptr) {
        return;
    }
    uint32_t seek = flags & SEEK_MASK;
    if (seek != SEEK_RELATIVE) {
        /* The ALSA plugin only ever appends.  Anything else would need a
         * rewindable render buffer, which we deliberately do not have. */
        HILOG_WARN(LOG_CORE, "stream %{public}u: ignoring seek mode %{public}u", channel, seek);
    }

    s->writeIndex += len;
    if (s->outstanding >= len) {
        s->outstanding -= len;
    } else {
        s->outstanding = 0;
    }

    size_t accepted = s->sink->Push(data, len);
    if (accepted < len) {
        /* Should not happen: we never ask for more than maxlength. */
        HILOG_WARN(LOG_CORE, "stream %{public}u: dropped %{public}zu B (ring full)", channel, len - accepted);
        int64_t zero = 0;
        (void)zero;
        SendEvent(c, COMMAND_OVERFLOW, channel, nullptr);
    }
    MaybeRequest(c, *s);
}

bool Server::HandleCommand(Connection &c, uint32_t command, uint32_t tag, TagReader &r)
{
    /* Everything except AUTH needs an authorised connection. */
    if (!c.authorized && command != COMMAND_AUTH) {
        SendError(c, tag, ERR_ACCESS);
        return true;
    }

    switch (command) {
        case COMMAND_AUTH: {
            uint32_t version = 0;
            std::string cookie;
            if (!r.GetU32(version) || !r.GetArbitrary(cookie)) {
                return false;
            }
            uint32_t peer = version & VERSION_MASK;
            if (peer < 8) {
                SendError(c, tag, ERR_VERSION);
                return true;
            }
            c.version = std::min(peer, PROTOCOL_VERSION);
            c.authorized = true;
            /*
             * We are a UNIX socket inside the device with no cookie to
             * share, so the cookie is ignored; the socket's own
             * permissions are the access control.  No SHM and no memfd
             * flags in the reply: that makes libpulse send sample data
             * inline, which is the only transport we implement.
             */
            HILOG_INFO(LOG_CORE, "client authorised, protocol v%{public}u (peer v%{public}u)",
                       c.version, peer);
            TagWriter t;
            SendReplyHeader(t, tag);
            t.PutU32(c.version);
            SendTag(c, t);
            return true;
        }
        case COMMAND_SET_CLIENT_NAME: {
            std::map<std::string, std::string> props;
            if (c.version >= 13) {
                if (!r.GetProplist(props)) {
                    return false;
                }
            } else {
                std::string name;
                if (!r.GetString(name)) {
                    return false;
                }
            }
            TagWriter t;
            SendReplyHeader(t, tag);
            if (c.version >= 13) {
                t.PutU32(nextClientIndex_++);
            }
            SendTag(c, t);
            return true;
        }
        case COMMAND_CREATE_PLAYBACK_STREAM:
            return HandleCreatePlayback(c, tag, r);

        case COMMAND_DELETE_PLAYBACK_STREAM: {
            uint32_t channel = 0;
            if (!r.GetU32(channel)) {
                return false;
            }
            auto it = c.streams.find(channel);
            if (it == c.streams.end()) {
                SendError(c, tag, ERR_NOENTITY);
                return true;
            }
            HILOG_INFO(LOG_CORE, "playback stream %{public}u gone", channel);
            c.streams.erase(it);
            SendSimpleAck(c, tag);
            return true;
        }
        case COMMAND_CORK_PLAYBACK_STREAM: {
            uint32_t channel = 0;
            bool cork = false;
            if (!r.GetU32(channel) || !r.GetBool(cork)) {
                return false;
            }
            Stream *s = FindStream(c, channel);
            if (s == nullptr) {
                SendError(c, tag, ERR_NOENTITY);
                return true;
            }
            s->corked = cork;
            s->sink->SetCorked(cork);
            SendSimpleAck(c, tag);
            MaybeRequest(c, *s);
            return true;
        }
        case COMMAND_FLUSH_PLAYBACK_STREAM: {
            uint32_t channel = 0;
            if (!r.GetU32(channel)) {
                return false;
            }
            Stream *s = FindStream(c, channel);
            if (s == nullptr) {
                SendError(c, tag, ERR_NOENTITY);
                return true;
            }
            s->sink->Flush();
            s->outstanding = 0;
            SendSimpleAck(c, tag);
            MaybeRequest(c, *s);
            return true;
        }
        case COMMAND_TRIGGER_PLAYBACK_STREAM:
        case COMMAND_PREBUF_PLAYBACK_STREAM: {
            uint32_t channel = 0;
            if (!r.GetU32(channel)) {
                return false;
            }
            Stream *s = FindStream(c, channel);
            if (s == nullptr) {
                SendError(c, tag, ERR_NOENTITY);
                return true;
            }
            /* We never hold audio back for prebuf, so both are already true. */
            SendSimpleAck(c, tag);
            return true;
        }
        case COMMAND_DRAIN_PLAYBACK_STREAM: {
            uint32_t channel = 0;
            if (!r.GetU32(channel)) {
                return false;
            }
            Stream *s = FindStream(c, channel);
            if (s == nullptr) {
                SendError(c, tag, ERR_NOENTITY);
                return true;
            }
            /* Acknowledged once the ring is empty and the renderer agrees. */
            s->drainPending = true;
            s->drainTag = tag;
            s->sink->RequestDrain();
            return true;
        }
        case COMMAND_GET_PLAYBACK_LATENCY: {
            uint32_t channel = 0;
            uint32_t sec = 0;
            uint32_t usec = 0;
            if (!r.GetU32(channel) || !r.GetTimeval(sec, usec)) {
                return false;
            }
            Stream *s = FindStream(c, channel);
            if (s == nullptr) {
                SendError(c, tag, ERR_NOENTITY);
                return true;
            }
            uint64_t readIndex = s->sink->ReadIndex();
            uint64_t now = NowUsec();
            TagWriter t;
            SendReplyHeader(t, tag);
            /*
             * sink_usec is the latency past our read index — what the
             * renderer still holds.  libpulse adds (write - read) itself,
             * so reporting both correctly is what keeps A/V in sync.
             */
            t.PutUsec(s->sink->SinkLatencyUsec());
            t.PutUsec(0); /* source_usec */
            t.PutBool(s->sink->IsPlaying());
            t.PutTimeval(sec, usec);
            t.PutTimeval(static_cast<uint32_t>(now / 1000000ULL), static_cast<uint32_t>(now % 1000000ULL));
            t.PutS64(static_cast<int64_t>(s->writeIndex));
            t.PutS64(static_cast<int64_t>(readIndex));
            if (c.version >= 13) {
                t.PutU64(s->underrunFor);
                t.PutU64(s->playingFor);
            }
            SendTag(c, t);
            return true;
        }
        case COMMAND_SET_PLAYBACK_STREAM_NAME: {
            uint32_t channel = 0;
            std::string name;
            if (!r.GetU32(channel) || !r.GetString(name)) {
                return false;
            }
            SendSimpleAck(c, tag);
            return true;
        }
        case COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR: {
            /*
             * Answer with the attributes we are actually keeping rather
             * than accept new ones: changing the ring under a running
             * render thread buys nothing here, and libpulse is happy to
             * be told the server kept its own numbers.
             */
            uint32_t channel = 0;
            uint32_t maxlength = 0;
            uint32_t tlength = 0;
            uint32_t prebuf = 0;
            uint32_t minreq = 0;
            if (!r.GetU32(channel) || !r.GetU32(maxlength) || !r.GetU32(tlength) ||
                !r.GetU32(prebuf) || !r.GetU32(minreq)) {
                return false;
            }
            Stream *s = FindStream(c, channel);
            if (s == nullptr) {
                SendError(c, tag, ERR_NOENTITY);
                return true;
            }
            TagWriter t;
            SendReplyHeader(t, tag);
            t.PutU32(s->attr.maxlength);
            t.PutU32(s->attr.tlength);
            t.PutU32(s->attr.prebuf);
            t.PutU32(s->attr.minreq);
            if (c.version >= 13) {
                t.PutUsec(s->sink->SinkLatencyUsec());
            }
            SendTag(c, t);
            return true;
        }
        case COMMAND_GET_SERVER_INFO: {
            TagWriter t;
            SendReplyHeader(t, tag);
            t.PutString("waydroid_audio");
            t.PutString("13.0.0");
            t.PutString("waydroid");
            t.PutString("localhost");
            SampleSpec ss;
            t.PutSampleSpec(ss);
            t.PutString(SINK_NAME);
            t.PutString(SOURCE_NAME);
            t.PutU32(0); /* cookie */
            if (c.version >= 15) {
                ChannelMap cm;
                t.PutChannelMap(cm);
            }
            SendTag(c, t);
            return true;
        }
        case COMMAND_CREATE_RECORD_STREAM:
            /* See the file comment: capture is out of scope. */
            HILOG_WARN(LOG_CORE, "record stream refused — capture is not implemented");
            SendError(c, tag, ERR_NOTSUPPORTED);
            return true;

        case COMMAND_EXIT:
            SendSimpleAck(c, tag);
            return true;

        default:
            HILOG_WARN(LOG_CORE, "unhandled command %{public}u", command);
            SendError(c, tag, ERR_NOTSUPPORTED);
            return true;
    }
}

bool Server::HandlePacket(Connection &c, uint32_t channel, uint32_t flags, uint64_t offset,
                          const uint8_t *data, size_t len)
{
    (void)offset;
    if (channel != CHANNEL_COMMAND) {
        HandleStreamData(c, channel, flags, data, len);
        return true;
    }
    if ((flags & FLAG_SHMDATA) != 0) {
        HILOG_ERROR(LOG_CORE, "client sent SHM data although we declined SHM");
        return false;
    }
    TagReader r(data, len);
    uint32_t command = 0;
    uint32_t tag = 0;
    if (!r.GetU32(command) || !r.GetU32(tag)) {
        HILOG_ERROR(LOG_CORE, "truncated command packet");
        return false;
    }
    return HandleCommand(c, command, tag, r);
}

/* Turn whatever the render threads did into REQUESTs and stream events. */
void Server::PumpSinks()
{
    for (auto &entry : conns_) {
        Connection &c = *entry.second;
        for (auto &se : c.streams) {
            Stream &s = *se.second;
            if (s.sink == nullptr) {
                continue;
            }
            OhosSink::Events ev = s.sink->TakeEvents();
            if (ev.credit > 0) {
                s.playingFor += s.ss.BytesToUsec(ev.credit);
            }
            if (ev.started) {
                SendEvent(c, COMMAND_STARTED, s.channel, nullptr);
            }
            if (ev.underflow) {
                s.underrunFor += 1;
                if (c.version >= 23) {
                    int64_t at = static_cast<int64_t>(s.sink->ReadIndex());
                    SendEvent(c, COMMAND_UNDERFLOW, s.channel, &at);
                } else {
                    SendEvent(c, COMMAND_UNDERFLOW, s.channel, nullptr);
                }
            }
            if (ev.drained && s.drainPending) {
                s.drainPending = false;
                SendSimpleAck(c, s.drainTag);
            }
            MaybeRequest(c, s);
        }
        UpdateEvents(c);
    }
}

bool Server::Start()
{
    /* The directory is the container's /run/xdg/pulse; create the whole
     * path because waydroidd only guarantees /run/xdg itself. */
    std::string dir = socketPath_.substr(0, socketPath_.find_last_of('/'));
    for (size_t i = 1; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            std::string part = dir.substr(0, i);
            if (mkdir(part.c_str(), 0777) < 0 && errno != EEXIST) {
                HILOG_ERROR(LOG_CORE, "mkdir %{public}s: %{public}s", part.c_str(), strerror(errno));
                return false;
            }
        }
    }
    (void)chmod(dir.c_str(), 0777);

    listenFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listenFd_ < 0) {
        HILOG_ERROR(LOG_CORE, "socket: %{public}s", strerror(errno));
        return false;
    }
    (void)unlink(socketPath_.c_str()); /* a stale socket from a dead generation */

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(addr.sun_path)) {
        HILOG_ERROR(LOG_CORE, "socket path too long: %{public}s", socketPath_.c_str());
        return false;
    }
    (void)strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(listenFd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        HILOG_ERROR(LOG_CORE, "bind %{public}s: %{public}s", socketPath_.c_str(), strerror(errno));
        return false;
    }
    (void)chmod(socketPath_.c_str(), 0666);
    if (listen(listenFd_, 8) < 0) {
        HILOG_ERROR(LOG_CORE, "listen: %{public}s", strerror(errno));
        return false;
    }

    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    eventFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (epollFd_ < 0 || eventFd_ < 0) {
        HILOG_ERROR(LOG_CORE, "epoll/eventfd: %{public}s", strerror(errno));
        return false;
    }
    struct epoll_event ev {};
    ev.events = EPOLLIN;
    ev.data.fd = listenFd_;
    (void)epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &ev);
    ev.data.fd = eventFd_;
    (void)epoll_ctl(epollFd_, EPOLL_CTL_ADD, eventFd_, &ev);

    HILOG_INFO(LOG_CORE, "listening on %{public}s", socketPath_.c_str());
    return true;
}

void Server::Accept()
{
    for (;;) {
        int fd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            return;
        }
        auto c = std::make_unique<Connection>();
        c->fd = fd;
        struct epoll_event ev {};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        (void)epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
        conns_[fd] = std::move(c);
        HILOG_INFO(LOG_CORE, "client connected (fd %{public}d)", fd);
    }
}

bool Server::ReadFrom(Connection &c)
{
    uint8_t buf[16384];
    for (;;) {
        ssize_t n = read(c.fd, buf, sizeof(buf));
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        c.in.insert(c.in.end(), buf, buf + n);
        if (static_cast<size_t>(n) < sizeof(buf)) {
            break;
        }
    }

    /* Drain as many whole packets as arrived. */
    size_t off = 0;
    while (c.in.size() - off >= DESCRIPTOR_SIZE) {
        const uint8_t *d = c.in.data() + off;
        auto be32 = [](const uint8_t *p) {
            return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        };
        uint32_t length = be32(d);
        uint32_t channel = be32(d + 4);
        uint64_t offset = (static_cast<uint64_t>(be32(d + 8)) << 32) | be32(d + 12);
        uint32_t flags = be32(d + 16);
        if (length > ABSOLUTE_MAX_BUFFER) {
            HILOG_ERROR(LOG_CORE, "absurd packet length %{public}u", length);
            return false;
        }
        if (c.in.size() - off < DESCRIPTOR_SIZE + length) {
            break; /* wait for the rest */
        }
        if (!HandlePacket(c, channel, flags, offset, d + DESCRIPTOR_SIZE, length)) {
            return false;
        }
        off += DESCRIPTOR_SIZE + length;
    }
    if (off > 0) {
        c.in.erase(c.in.begin(), c.in.begin() + off);
    }
    return true;
}

bool Server::FlushOut(Connection &c)
{
    size_t sent = 0;
    while (sent < c.out.size()) {
        ssize_t n = write(c.fd, c.out.data() + sent, c.out.size() - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    if (sent > 0) {
        c.out.erase(c.out.begin(), c.out.begin() + sent);
    }
    return true;
}

void Server::UpdateEvents(Connection &c)
{
    struct epoll_event ev {};
    ev.events = EPOLLIN | (c.out.empty() ? 0 : EPOLLOUT);
    ev.data.fd = c.fd;
    (void)epoll_ctl(epollFd_, EPOLL_CTL_MOD, c.fd, &ev);
}

void Server::Drop(int fd)
{
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    HILOG_INFO(LOG_CORE, "client gone (fd %{public}d)", fd);
    (void)epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    conns_.erase(it);
    (void)close(fd);
}

void Server::Run()
{
    struct epoll_event events[32];
    while (g_stop == 0) {
        int n = epoll_wait(epollFd_, events, 32, 200);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        std::vector<int> dead;
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listenFd_) {
                Accept();
                continue;
            }
            if (fd == eventFd_) {
                uint64_t v = 0;
                ssize_t ignored = read(eventFd_, &v, sizeof(v));
                (void)ignored;
                continue;
            }
            auto it = conns_.find(fd);
            if (it == conns_.end()) {
                continue;
            }
            Connection &c = *it->second;
            if ((events[i].events & (EPOLLHUP | EPOLLERR)) != 0) {
                dead.push_back(fd);
                continue;
            }
            if ((events[i].events & EPOLLIN) != 0 && !ReadFrom(c)) {
                dead.push_back(fd);
                continue;
            }
            if ((events[i].events & EPOLLOUT) != 0 && !FlushOut(c)) {
                dead.push_back(fd);
                continue;
            }
        }
        for (int fd : dead) {
            Drop(fd);
        }

        /* Credit, events and any queued output, every turn of the loop. */
        PumpSinks();
        std::vector<int> broken;
        for (auto &entry : conns_) {
            if (!FlushOut(*entry.second)) {
                broken.push_back(entry.first);
            } else {
                UpdateEvents(*entry.second);
            }
        }
        for (int fd : broken) {
            Drop(fd);
        }
    }
    HILOG_INFO(LOG_CORE, "stopping");
}

void Server::Shutdown()
{
    conns_.clear();
    if (listenFd_ >= 0) {
        (void)close(listenFd_);
        listenFd_ = -1;
    }
    if (epollFd_ >= 0) {
        (void)close(epollFd_);
        epollFd_ = -1;
    }
    if (eventFd_ >= 0) {
        (void)close(eventFd_);
        eventFd_ = -1;
    }
    (void)unlink(socketPath_.c_str());
}

} // namespace
} // namespace waydroid

int main(int argc, char **argv)
{
    std::string path = waydroid::DEFAULT_SOCKET;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else {
            (void)fprintf(stderr, "usage: %s [--socket <path>]\n", argv[0]);
            return 2;
        }
    }

    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGINT, waydroid::OnSignal);
    (void)signal(SIGTERM, waydroid::OnSignal);

    waydroid::Server server(path);
    if (!server.Start()) {
        return 1;
    }
    server.Run();
    return 0;
}
