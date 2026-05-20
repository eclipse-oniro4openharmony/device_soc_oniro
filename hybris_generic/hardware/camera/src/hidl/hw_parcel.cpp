/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hw_parcel.h"

#include <cassert>
#include <cstring>

namespace OHOS::Camera::Hybris::Hidl {

namespace {

constexpr size_t Round(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/*
 * HidlString in-memory layout — has to match
 * system/libhidl/base/include/hidl/HidlSupport.h on 64-bit AOSP.
 *   off  size  field
 *    0   8     const char *buffer       (kernel rewrites this)
 *    8   4     uint32_t    size         (length, no NUL)
 *   12   1     uint8_t     ownsBuffer
 *   13   3     padding
 *   total: 16 bytes, 8-byte aligned.
 */
struct HidlStringLayout {
    uint64_t buffer;
    uint32_t size;
    uint8_t  owns;
    uint8_t  pad[3];
};
static_assert(sizeof(HidlStringLayout) == 16, "HidlString must be 16 bytes");

constexpr size_t kBufferFieldOffset = 0; /* offset of `buffer` in HidlStringLayout */

} // namespace

void HwParcel::AlignTo(size_t alignment)
{
    size_t aligned = Round(data_.size(), alignment);
    if (aligned > data_.size()) {
        data_.resize(aligned, 0);
    }
}

void HwParcel::WriteRaw(const void *p, size_t n, size_t alignTo)
{
    AlignTo(alignTo);
    size_t pos = data_.size();
    data_.resize(pos + n);
    memcpy(data_.data() + pos, p, n);
}

size_t HwParcel::AppendBufferObject(const binder_buffer_object &bo)
{
    /*
     * Do NOT pre-align to 8 here.  AOSP libhwbinder writeBuffer appends
     * a binder_buffer_object directly at the current mDataPos with no
     * 8-byte realignment, so the receiver's readBuffer reads buffer
     * objects sequentially from mDataPos (advanced by 40 each call).
     * Inserting 8-byte padding here puts the object 4 bytes after where
     * the receiver expects it, and the (zero-padding) interpreted as
     * hdr.type fails the BINDER_TYPE_PTR check → BAD_VALUE (-22).
     *
     * Discovered via libhidlbase Parcel::readBuffer disassembly +
     * a hex-dump of a failing listByInterface(fqName) call
     * (2026-05-21).  IBase descriptors happen to be 8-aligned post-pad,
     * which is why ping worked despite this bug.
     */
    size_t off = data_.size();
    data_.resize(off + sizeof(bo));
    memcpy(data_.data() + off, &bo, sizeof(bo));
    offsets_.push_back(off);
    return offsets_.size() - 1;
}

void HwParcel::GrowSg(size_t n)
{
    sgSize_ += Round(n, 8);
}

void HwParcel::WriteInterfaceToken(const char *iface)
{
    /*
     * Flat null-terminated C string + 4-byte alignment padding.
     * Receiver enforceInterface() does memchr+strcmp; no offsets, no
     * SG.  See phase_n12_camera.md § N12.5 "wire-format gotcha #2".
     */
    const size_t len = strlen(iface);
    const size_t total = Round(len + 1, 4);
    size_t pos = data_.size();
    data_.resize(pos + total, 0);
    memcpy(data_.data() + pos, iface, len);
}

void HwParcel::WriteHidlString(const std::string &s)
{
    /*
     * (1) Stage the HidlString struct in our owned SG memory.  The
     *     `buffer` field is a placeholder — the kernel rewrites it to
     *     the receiver-side address during fixup.
     */
    auto strucBlob = std::make_unique<std::vector<uint8_t>>(sizeof(HidlStringLayout), 0);
    auto *layout = reinterpret_cast<HidlStringLayout *>(strucBlob->data());
    layout->buffer = 0;
    layout->size = static_cast<uint32_t>(s.size());
    layout->owns = 0;

    /* (2) Stage the string bytes (NUL-terminated). */
    auto bytesBlob = std::make_unique<std::vector<uint8_t>>(s.size() + 1, 0);
    memcpy(bytesBlob->data(), s.data(), s.size());

    /* (3) First binder_buffer_object: wraps the HidlString struct. */
    binder_buffer_object boStruct = {};
    boStruct.hdr.type = BINDER_TYPE_PTR;
    boStruct.flags = 0;
    boStruct.buffer = reinterpret_cast<uintptr_t>(strucBlob->data());
    boStruct.length = sizeof(HidlStringLayout);
    boStruct.parent = 0;
    boStruct.parent_offset = 0;
    size_t parentIdx = AppendBufferObject(boStruct);
    GrowSg(sizeof(HidlStringLayout));

    /* (4) Second binder_buffer_object: wraps the string bytes, with a
     *     parent fixup pointing into the HidlString::buffer field. */
    binder_buffer_object boBytes = {};
    boBytes.hdr.type = BINDER_TYPE_PTR;
    boBytes.flags = BINDER_BUFFER_FLAG_HAS_PARENT;
    boBytes.buffer = reinterpret_cast<uintptr_t>(bytesBlob->data());
    boBytes.length = s.size() + 1;
    boBytes.parent = parentIdx;
    boBytes.parent_offset = kBufferFieldOffset;
    AppendBufferObject(boBytes);
    GrowSg(s.size() + 1);

    ownedBlobs_.push_back(std::move(strucBlob));
    ownedBlobs_.push_back(std::move(bytesBlob));
}

/* ─── Reader ──────────────────────────────────────────────────────── */

bool HwParcel::Reader::Skip(size_t n)
{
    if (cursor_ + n > dataSize_) return false;
    cursor_ += n;
    return true;
}

bool HwParcel::Reader::ReadInt32(int32_t *out)
{
    /* HIDL primitives are 4-byte aligned within data_. */
    cursor_ = Round(cursor_, 4);
    if (cursor_ + sizeof(int32_t) > dataSize_) return false;
    memcpy(out, data_ + cursor_, sizeof(int32_t));
    cursor_ += sizeof(int32_t);
    return true;
}

bool HwParcel::Reader::ReadUint32(uint32_t *out)
{
    return ReadInt32(reinterpret_cast<int32_t *>(out));
}

bool HwParcel::Reader::ReadInt64(int64_t *out)
{
    cursor_ = Round(cursor_, 8);
    if (cursor_ + sizeof(int64_t) > dataSize_) return false;
    memcpy(out, data_ + cursor_, sizeof(int64_t));
    cursor_ += sizeof(int64_t);
    return true;
}

bool HwParcel::Reader::ReadUint64(uint64_t *out)
{
    return ReadInt64(reinterpret_cast<int64_t *>(out));
}

bool HwParcel::Reader::ReadFlatBinder(uint32_t *outHandle, bool *isNull)
{
    /* libhwbinder primitives are 4-byte aligned; flat_binder_object is
     * written at the current mDataPos with no realignment (writeObject
     * just appends sizeof(flat_binder_object)=24 bytes).  Same lesson
     * as AppendBufferObject in the writer. */
    cursor_ = Round(cursor_, 4);
    if (cursor_ + sizeof(flat_binder_object) > dataSize_) return false;
    flat_binder_object fbo;
    memcpy(&fbo, data_ + cursor_, sizeof(fbo));
    cursor_ += sizeof(fbo);

    /*
     * The kernel rewrites BINDER_TYPE_BINDER → BINDER_TYPE_HANDLE when
     * delivering a binder ref across processes.  But a *null* strong
     * binder isn't rewritten — the sender writes BINDER_TYPE_BINDER
     * with binder=cookie=0 to mean null, and that's what we see on
     * the receiver side too.  So treat (type==BINDER, handle==0) AND
     * (type==HANDLE, handle==0) AND (type==0, handle==0) all as null.
     */
    const bool isHandle = fbo.hdr.type == (uint32_t)BINDER_TYPE_HANDLE;
    const bool isBinder = fbo.hdr.type == (uint32_t)BINDER_TYPE_BINDER;
    if ((isHandle || isBinder || fbo.hdr.type == 0) && fbo.handle == 0) {
        *isNull = true;
        *outHandle = 0;
        return true;
    }
    if (!isHandle) {
        *isNull = false;
        *outHandle = 0;
        return false;
    }
    *isNull = false;
    *outHandle = fbo.handle;
    return true;
}

namespace {

struct HidlVecLayout {
    uint64_t buffer;     /* T* (kernel-fixed-up) */
    uint32_t size;       /* count of elements */
    uint8_t  owns;
    uint8_t  pad[3];
};
static_assert(sizeof(HidlVecLayout) == 16, "HidlVec must be 16 bytes");

struct HidlStringReplyLayout {
    uint64_t buffer;     /* const char* (kernel-fixed-up) */
    uint32_t size;       /* byte length, excluding NUL */
    uint8_t  owns;
    uint8_t  pad[3];
};
static_assert(sizeof(HidlStringReplyLayout) == 16,
              "HidlString reply layout must be 16 bytes");

} // namespace

bool HwParcel::Reader::ReadHidlStringVec(std::vector<std::string> *out)
{
    /*
     * Wire format for hidl_vec<hidl_string> in a reply parcel:
     *
     *   main data:
     *     [+0]   binder_buffer_object#0   PTR → hidl_vec struct in SG
     *     [+40]  binder_buffer_object#1   PTR → array of hidl_strings, with
     *                                     HAS_PARENT pointing into #0
     *     [+80]  binder_buffer_object#2   PTR → element[0] string bytes
     *     [+120] binder_buffer_object#3   PTR → element[1] string bytes
     *     ...
     *
     * Each PTR's .buffer is a kernel-fixed-up address pointing inside
     * our mmap'd SG region.  The hidl_vec/hidl_string structs in SG
     * have been fixup'd too: their inner pointers (.buffer fields)
     * already point at their referent in SG.  So we can dereference
     * directly, no manual fixup walking.
     */
    if (cursor_ + sizeof(binder_buffer_object) > dataSize_) return false;
    binder_buffer_object boVec;
    memcpy(&boVec, data_ + cursor_, sizeof(boVec));
    cursor_ += sizeof(boVec);
    if (boVec.hdr.type != (uint32_t)BINDER_TYPE_PTR) return false;
    if (boVec.length != sizeof(HidlVecLayout))      return false;
    if (boVec.buffer == 0)                           return false;

    HidlVecLayout vec;
    memcpy(&vec, reinterpret_cast<const void *>(boVec.buffer), sizeof(vec));

    if (cursor_ + sizeof(binder_buffer_object) > dataSize_) return false;
    binder_buffer_object boArr;
    memcpy(&boArr, data_ + cursor_, sizeof(boArr));
    cursor_ += sizeof(boArr);
    if (boArr.hdr.type != (uint32_t)BINDER_TYPE_PTR) return false;
    /* boArr.length == size * sizeof(HidlStringReplyLayout) */

    out->clear();
    out->reserve(vec.size);

    const HidlStringReplyLayout *arr =
        reinterpret_cast<const HidlStringReplyLayout *>(boArr.buffer);

    for (uint32_t i = 0; i < vec.size; ++i) {
        if (cursor_ + sizeof(binder_buffer_object) > dataSize_) return false;
        binder_buffer_object boStr;
        memcpy(&boStr, data_ + cursor_, sizeof(boStr));
        cursor_ += sizeof(boStr);
        if (boStr.hdr.type != (uint32_t)BINDER_TYPE_PTR) return false;

        if (arr[i].buffer == 0 || arr[i].size == 0) {
            out->emplace_back();
        } else {
            out->emplace_back(
                reinterpret_cast<const char *>(arr[i].buffer), arr[i].size);
        }
    }
    return true;
}

bool HwParcel::Reader::ReadHidlString(std::string *out)
{
    /*
     * Reply embeds a HidlString struct in the main data; the kernel
     * has already rewritten `buffer` to live in our mmap'd SG region.
     * We also need to consume the (binder_buffer_object) header that
     * preceded the struct — but on the receiver side the kernel hides
     * the buffer objects from the data stream by replacing them with
     * the embedded struct contents.  In practice, libhwbinder's
     * Parcel::readEmbeddedBuffer skips the buffer_object at
     * offsets_[N] and reads the HidlString struct that follows.
     *
     * For the simple replies we care about here (status + flat
     * payload), the caller knows the shape and can position the cursor
     * correctly.  This minimal implementation reads a HidlStringLayout
     * sitting at the current cursor.
     */
    cursor_ = Round(cursor_, 8);
    if (cursor_ + sizeof(HidlStringLayout) > dataSize_) return false;
    HidlStringLayout layout;
    memcpy(&layout, data_ + cursor_, sizeof(layout));
    cursor_ += sizeof(layout);
    if (layout.size == 0 || layout.buffer == 0) {
        out->clear();
        return true;
    }
    out->assign(reinterpret_cast<const char *>(layout.buffer), layout.size);
    return true;
}

} // namespace OHOS::Camera::Hybris::Hidl
