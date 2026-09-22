/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * pa_wire.h — the PulseAudio native protocol on the wire.
 *
 * Only the encoding lives here: the 20-byte packet descriptor and the
 * "tagstruct" (PulseAudio's typed, self-describing serialisation).  Who
 * says what to whom is waydroid_audio_main.cpp.
 *
 * The layouts are not invented: they are read off the pulseaudio sources
 * that OHOS already vendors at //third_party/pulseaudio —
 * src/pulsecore/{tagstruct,pstream,native-common}.{c,h} and, for the
 * command payloads, the client half in src/pulse/{context,stream}.c.
 * That matters because the peer is a real libpulse (the container's
 * audio.primary.waydroid reaches us through alsa-lib's pulse plugin), so
 * every field has to land where libpulse expects to read it.
 *
 * We speak protocol version 13.  libpulse always negotiates down to the
 * server's version, v13 is the oldest that carries everything the ALSA
 * plugin uses (proplists, ADJUST_LATENCY, configured_sink_usec), and its
 * payloads are the shortest that do.  Deliberately absent from our reply
 * flags: SHM and memfd.  Declining both is what makes libpulse send the
 * audio inline down the socket instead of through a shared mempool we
 * would otherwise have to implement.
 */

#ifndef WAYDROID_PA_WIRE_H
#define WAYDROID_PA_WIRE_H

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace waydroid {
namespace pa {

/* The version we advertise; see the file comment. */
constexpr uint32_t PROTOCOL_VERSION = 13;
constexpr uint32_t VERSION_MASK = 0x0000FFFFu;
constexpr uint32_t FLAG_SHM = 0x80000000u;
constexpr uint32_t FLAG_MEMFD = 0x40000000u;

constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;
constexpr size_t COOKIE_LENGTH = 256;

/* src/pulsecore/native-common.h.  Only the ones we handle are named. */
enum Command : uint32_t {
    COMMAND_ERROR = 0,
    COMMAND_TIMEOUT = 1,
    COMMAND_REPLY = 2,
    COMMAND_CREATE_PLAYBACK_STREAM = 3,
    COMMAND_DELETE_PLAYBACK_STREAM = 4,
    COMMAND_CREATE_RECORD_STREAM = 5,
    COMMAND_DELETE_RECORD_STREAM = 6,
    COMMAND_EXIT = 7,
    COMMAND_AUTH = 8,
    COMMAND_SET_CLIENT_NAME = 9,
    COMMAND_LOOKUP_SINK = 10,
    COMMAND_LOOKUP_SOURCE = 11,
    COMMAND_DRAIN_PLAYBACK_STREAM = 12,
    COMMAND_STAT = 13,
    COMMAND_GET_PLAYBACK_LATENCY = 14,
    COMMAND_GET_SERVER_INFO = 20,
    COMMAND_GET_SINK_INFO = 21,
    COMMAND_GET_SINK_INFO_LIST = 22,
    COMMAND_GET_SOURCE_INFO = 23,
    COMMAND_GET_SOURCE_INFO_LIST = 24,
    COMMAND_SUBSCRIBE = 35,
    COMMAND_SET_SINK_VOLUME = 36,
    COMMAND_SET_SINK_INPUT_VOLUME = 37,
    COMMAND_SET_SINK_MUTE = 39,
    COMMAND_CORK_PLAYBACK_STREAM = 41,
    COMMAND_FLUSH_PLAYBACK_STREAM = 42,
    COMMAND_TRIGGER_PLAYBACK_STREAM = 43,
    COMMAND_SET_PLAYBACK_STREAM_NAME = 46,
    COMMAND_SET_RECORD_STREAM_NAME = 47,
    COMMAND_GET_RECORD_LATENCY = 57,
    COMMAND_CORK_RECORD_STREAM = 58,
    COMMAND_FLUSH_RECORD_STREAM = 59,
    COMMAND_PREBUF_PLAYBACK_STREAM = 60,
    /* server -> client */
    COMMAND_REQUEST = 61,
    COMMAND_OVERFLOW = 62,
    COMMAND_UNDERFLOW = 63,
    COMMAND_PLAYBACK_STREAM_KILLED = 64,
    COMMAND_RECORD_STREAM_KILLED = 65,
    COMMAND_SUBSCRIBE_EVENT = 66,
    COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR = 72,
    COMMAND_SET_RECORD_STREAM_BUFFER_ATTR = 73,
    COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE = 74,
    COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE = 75,
    COMMAND_STARTED = 86,
};

/* src/pulse/def.h — the subset we can return. */
enum Error : uint32_t {
    ERR_OK = 0,
    ERR_ACCESS = 1,
    ERR_COMMAND = 2,
    ERR_INVALID = 3,
    ERR_EXIST = 4,
    ERR_NOENTITY = 5,
    ERR_VERSION = 17,
    ERR_NOTSUPPORTED = 19,
};

/* src/pulse/sample.h */
enum SampleFormat : uint8_t {
    SAMPLE_U8 = 0,
    SAMPLE_ALAW = 1,
    SAMPLE_ULAW = 2,
    SAMPLE_S16LE = 3,
    SAMPLE_S16BE = 4,
    SAMPLE_FLOAT32LE = 5,
    SAMPLE_FLOAT32BE = 6,
    SAMPLE_S32LE = 7,
    SAMPLE_S32BE = 8,
    SAMPLE_S24LE = 9,
    SAMPLE_S24BE = 10,
    SAMPLE_S24_32LE = 11,
    SAMPLE_S24_32BE = 12,
    SAMPLE_INVALID = 0xFF,
};

/* src/pulsecore/tagstruct.h */
enum Tag : uint8_t {
    TAG_STRING = 't',
    TAG_STRING_NULL = 'N',
    TAG_U32 = 'L',
    TAG_U8 = 'B',
    TAG_U64 = 'R',
    TAG_S64 = 'r',
    TAG_SAMPLE_SPEC = 'a',
    TAG_ARBITRARY = 'x',
    TAG_BOOLEAN_TRUE = '1',
    TAG_BOOLEAN_FALSE = '0',
    TAG_TIMEVAL = 'T',
    TAG_USEC = 'U',
    TAG_CHANNEL_MAP = 'm',
    TAG_CVOLUME = 'v',
    TAG_PROPLIST = 'P',
    TAG_VOLUME = 'V',
    TAG_FORMAT_INFO = 'f',
};

constexpr uint32_t VOLUME_NORM = 0x10000u;

/* src/pulse/channelmap.h */
enum ChannelPosition : uint8_t {
    CHANNEL_MONO = 0,
    CHANNEL_FRONT_LEFT = 1,
    CHANNEL_FRONT_RIGHT = 2,
};

/*
 * src/pulsecore/pstream.c: every packet is five big-endian uint32 —
 * length, channel, offset_hi, offset_lo, flags — then `length` bytes.
 * channel == -1 marks a command packet (a tagstruct); anything else is
 * sample data for that stream's channel.
 */
constexpr size_t DESCRIPTOR_SIZE = 20;
constexpr uint32_t CHANNEL_COMMAND = 0xFFFFFFFFu;
constexpr uint32_t FLAG_SHMDATA = 0x80000000u;
/* The low byte of a data packet's flags is the seek mode (src/pulse/def.h). */
constexpr uint32_t SEEK_MASK = 0x000000FFu;
enum SeekMode : uint32_t {
    SEEK_RELATIVE = 0,
    SEEK_ABSOLUTE = 1,
    SEEK_RELATIVE_ON_READ = 2,
    SEEK_RELATIVE_END = 3,
};

struct SampleSpec {
    uint8_t format = SAMPLE_S16LE;
    uint8_t channels = 2;
    uint32_t rate = 48000;

    size_t FrameSize() const
    {
        size_t width;
        switch (format) {
            case SAMPLE_U8:
            case SAMPLE_ALAW:
            case SAMPLE_ULAW: width = 1; break;
            case SAMPLE_S16LE:
            case SAMPLE_S16BE: width = 2; break;
            case SAMPLE_S24LE:
            case SAMPLE_S24BE: width = 3; break;
            default: width = 4; break;
        }
        return width * channels;
    }
    bool Valid() const
    {
        return channels > 0 && channels <= 32 && rate > 0 && rate <= 384000 &&
               format <= SAMPLE_S24_32BE;
    }
    uint64_t BytesToUsec(uint64_t bytes) const
    {
        size_t fs = FrameSize();
        if (fs == 0 || rate == 0) {
            return 0;
        }
        return (bytes / fs) * 1000000ULL / rate;
    }
    uint64_t UsecToBytes(uint64_t usec) const
    {
        return (usec * rate / 1000000ULL) * FrameSize();
    }
};

struct ChannelMap {
    uint8_t channels = 2;
    uint8_t map[32] = { CHANNEL_FRONT_LEFT, CHANNEL_FRONT_RIGHT };
};

/* PulseAudio's per-stream buffer contract (src/pulse/def.h, pa_buffer_attr). */
struct BufferAttr {
    uint32_t maxlength = INVALID_INDEX;
    uint32_t tlength = INVALID_INDEX;   /* playback: target fill */
    uint32_t prebuf = INVALID_INDEX;
    uint32_t minreq = INVALID_INDEX;
    uint32_t fragsize = INVALID_INDEX;  /* record */
};

class TagWriter {
public:
    void PutU8(uint8_t v) { Raw8(TAG_U8); Raw8(v); }
    void PutU32(uint32_t v) { Raw8(TAG_U32); Raw32(v); }
    void PutU64(uint64_t v) { Raw8(TAG_U64); Raw64(v); }
    void PutS64(int64_t v) { Raw8(TAG_S64); Raw64(static_cast<uint64_t>(v)); }
    void PutUsec(uint64_t v) { Raw8(TAG_USEC); Raw64(v); }
    void PutVolume(uint32_t v) { Raw8(TAG_VOLUME); Raw32(v); }
    void PutBool(bool v) { Raw8(v ? TAG_BOOLEAN_TRUE : TAG_BOOLEAN_FALSE); }

    void PutString(const char *s)
    {
        if (s == nullptr) {
            Raw8(TAG_STRING_NULL);
            return;
        }
        Raw8(TAG_STRING);
        buf_.insert(buf_.end(), s, s + strlen(s) + 1);
    }

    void PutSampleSpec(const SampleSpec &ss)
    {
        Raw8(TAG_SAMPLE_SPEC);
        Raw8(ss.format);
        Raw8(ss.channels);
        Raw32(ss.rate);
    }

    void PutChannelMap(const ChannelMap &cm)
    {
        Raw8(TAG_CHANNEL_MAP);
        Raw8(cm.channels);
        for (uint8_t i = 0; i < cm.channels; i++) {
            Raw8(cm.map[i]);
        }
    }

    void PutCVolume(uint8_t channels, uint32_t value)
    {
        Raw8(TAG_CVOLUME);
        Raw8(channels);
        for (uint8_t i = 0; i < channels; i++) {
            Raw32(value);
        }
    }

    void PutTimeval(uint32_t sec, uint32_t usec)
    {
        Raw8(TAG_TIMEVAL);
        Raw32(sec);
        Raw32(usec);
    }

    /* An empty proplist is the tag plus the terminating NULL string. */
    void PutEmptyProplist() { Raw8(TAG_PROPLIST); Raw8(TAG_STRING_NULL); }

    void PutProplist(const std::map<std::string, std::string> &props)
    {
        Raw8(TAG_PROPLIST);
        for (const auto &kv : props) {
            PutString(kv.first.c_str());
            /* PulseAudio stores the value's length, then the value as an
             * "arbitrary" blob, NUL included. */
            PutU32(static_cast<uint32_t>(kv.second.size() + 1));
            Raw8(TAG_ARBITRARY);
            Raw32(static_cast<uint32_t>(kv.second.size() + 1));
            buf_.insert(buf_.end(), kv.second.begin(), kv.second.end());
            buf_.push_back(0);
        }
        Raw8(TAG_STRING_NULL);
    }

    const std::vector<uint8_t> &Data() const { return buf_; }
    size_t Size() const { return buf_.size(); }

private:
    void Raw8(uint8_t v) { buf_.push_back(v); }
    void Raw32(uint32_t v)
    {
        buf_.push_back(static_cast<uint8_t>(v >> 24));
        buf_.push_back(static_cast<uint8_t>(v >> 16));
        buf_.push_back(static_cast<uint8_t>(v >> 8));
        buf_.push_back(static_cast<uint8_t>(v));
    }
    void Raw64(uint64_t v) { Raw32(static_cast<uint32_t>(v >> 32)); Raw32(static_cast<uint32_t>(v)); }

    std::vector<uint8_t> buf_;
};

class TagReader {
public:
    TagReader(const uint8_t *data, size_t len) : data_(data), len_(len) {}

    bool GetU8(uint8_t &out) { return Expect(TAG_U8) && Take8(out); }
    bool GetU32(uint32_t &out) { return Expect(TAG_U32) && Take32(out); }
    bool GetU64(uint64_t &out) { return Expect(TAG_U64) && Take64(out); }

    bool GetS64(int64_t &out)
    {
        uint64_t v;
        if (!Expect(TAG_S64) || !Take64(v)) {
            return false;
        }
        out = static_cast<int64_t>(v);
        return true;
    }

    bool GetUsec(uint64_t &out) { return Expect(TAG_USEC) && Take64(out); }

    bool GetBool(bool &out)
    {
        uint8_t tag;
        if (!Take8(tag)) {
            return false;
        }
        if (tag == TAG_BOOLEAN_TRUE) {
            out = true;
        } else if (tag == TAG_BOOLEAN_FALSE) {
            out = false;
        } else {
            return Fail();
        }
        return true;
    }

    /* A NULL string is a distinct tag; it reads back as the empty string
     * with isNull set, which is how "no device given" arrives. */
    bool GetString(std::string &out, bool *isNull = nullptr)
    {
        uint8_t tag;
        if (!Take8(tag)) {
            return false;
        }
        if (tag == TAG_STRING_NULL) {
            out.clear();
            if (isNull != nullptr) {
                *isNull = true;
            }
            return true;
        }
        if (tag != TAG_STRING) {
            return Fail();
        }
        if (isNull != nullptr) {
            *isNull = false;
        }
        size_t start = off_;
        while (off_ < len_ && data_[off_] != 0) {
            off_++;
        }
        if (off_ >= len_) {
            return Fail();
        }
        out.assign(reinterpret_cast<const char *>(data_ + start), off_ - start);
        off_++; /* the NUL */
        return true;
    }

    bool GetSampleSpec(SampleSpec &ss)
    {
        return Expect(TAG_SAMPLE_SPEC) && Take8(ss.format) && Take8(ss.channels) && Take32(ss.rate);
    }

    bool GetChannelMap(ChannelMap &cm)
    {
        if (!Expect(TAG_CHANNEL_MAP) || !Take8(cm.channels)) {
            return false;
        }
        if (cm.channels > sizeof(cm.map)) {
            return Fail();
        }
        for (uint8_t i = 0; i < cm.channels; i++) {
            if (!Take8(cm.map[i])) {
                return false;
            }
        }
        return true;
    }

    bool GetCVolume(uint8_t &channels, uint32_t &first)
    {
        if (!Expect(TAG_CVOLUME) || !Take8(channels)) {
            return false;
        }
        first = VOLUME_NORM;
        for (uint8_t i = 0; i < channels; i++) {
            uint32_t v;
            if (!Take32(v)) {
                return false;
            }
            if (i == 0) {
                first = v;
            }
        }
        return true;
    }

    bool GetTimeval(uint32_t &sec, uint32_t &usec)
    {
        return Expect(TAG_TIMEVAL) && Take32(sec) && Take32(usec);
    }

    bool GetArbitrary(std::string &out)
    {
        uint32_t len;
        if (!Expect(TAG_ARBITRARY) || !Take32(len)) {
            return false;
        }
        if (len > len_ - off_) {
            return Fail();
        }
        out.assign(reinterpret_cast<const char *>(data_ + off_), len);
        off_ += len;
        return true;
    }

    bool GetProplist(std::map<std::string, std::string> &props)
    {
        if (!Expect(TAG_PROPLIST)) {
            return false;
        }
        for (;;) {
            if (off_ >= len_) {
                return Fail();
            }
            if (data_[off_] == TAG_STRING_NULL) {
                off_++;
                return true;
            }
            std::string key;
            uint32_t declared;
            std::string value;
            if (!GetString(key) || !GetU32(declared) || !GetArbitrary(value)) {
                return false;
            }
            /* Values are usually NUL-terminated text; drop the NUL. */
            if (!value.empty() && value.back() == '\0') {
                value.pop_back();
            }
            props[key] = value;
        }
    }

    bool Eof() const { return !failed_ && off_ == len_; }
    bool Failed() const { return failed_; }
    size_t Remaining() const { return len_ - off_; }

private:
    bool Fail() { failed_ = true; return false; }
    bool Expect(uint8_t want)
    {
        uint8_t tag;
        if (!Take8(tag)) {
            return false;
        }
        return tag == want ? true : Fail();
    }
    bool Take8(uint8_t &out)
    {
        if (failed_ || off_ + 1 > len_) {
            return Fail();
        }
        out = data_[off_++];
        return true;
    }
    bool Take32(uint32_t &out)
    {
        if (failed_ || off_ + 4 > len_) {
            return Fail();
        }
        out = (static_cast<uint32_t>(data_[off_]) << 24) | (static_cast<uint32_t>(data_[off_ + 1]) << 16) |
              (static_cast<uint32_t>(data_[off_ + 2]) << 8) | static_cast<uint32_t>(data_[off_ + 3]);
        off_ += 4;
        return true;
    }
    bool Take64(uint64_t &out)
    {
        uint32_t hi, lo;
        if (!Take32(hi) || !Take32(lo)) {
            return false;
        }
        out = (static_cast<uint64_t>(hi) << 32) | lo;
        return true;
    }

    const uint8_t *data_;
    size_t len_;
    size_t off_ = 0;
    bool failed_ = false;
};

} // namespace pa
} // namespace waydroid

#endif /* WAYDROID_PA_WIRE_H */
