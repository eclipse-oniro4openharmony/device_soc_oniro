/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * HwParcel — minimal HIDL Parcel encoder/decoder for the pure-musl
 * transport library.  Wire format mirrors AOSP libhwbinder/Parcel +
 * libhidlbase/HidlSupport hidl_string layout.  See phase_n12_camera.md
 * § N12.5 for the design and three wire-format gotchas.
 */

#ifndef HYBRIS_HIDL_HW_PARCEL_H
#define HYBRIS_HIDL_HW_PARCEL_H

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hw_binder_abi.h"

namespace OHOS::Camera::Hybris::Hidl {

/*
 * HwParcel encodes a HIDL request.  Two storage areas back it:
 *
 *   - `data_`     — the "main" buffer that goes through binder.  Holds
 *                   primitives and binder_buffer_object headers.
 *   - `sg_`       — the "scatter-gather" secondary buffer the kernel
 *                   allocates next to the main buffer in the receiver's
 *                   mmap region.  Holds the contents that
 *                   binder_buffer_objects in `data_` point at (HidlString
 *                   structs, string bytes, hidl_vec backing arrays, …).
 *
 * `offsets_` records the byte offset of every binder object inside
 * `data_` and gets passed to the kernel alongside the main buffer.
 *
 * Storage backing pointers for SG buffers lives in `ownedBlobs_` so the
 * raw user-space addresses we hand to the kernel stay valid for the
 * lifetime of the parcel (until transact() returns).
 *
 * Reader side: a `HwParcel::Reader` wraps a const view of the reply
 * buffer the kernel handed us in BR_REPLY (already fixed-up; pointers
 * inside it are valid addresses in our mmap'd region).
 */
class HwParcel {
public:
    HwParcel() = default;

    /* ─── Writers ─────────────────────────────────────────────────── */

    /*
     * Write a raw interface-token: the bytes of `iface`, a NUL
     * terminator, and 4-byte alignment padding.  NOT a HidlString —
     * Parcel::writeInterfaceToken in libhidlbase writes a flat C string
     * (memchr+strcmp in the receiver's enforceInterface()).  Verified
     * by disassembly 2026-05-21.
     */
    void WriteInterfaceToken(const char *iface);

    /* Primitives — all 4-byte aligned in main buffer (HIDL convention). */
    void WriteInt32(int32_t v)   { WriteRaw(&v, sizeof(v), 4); }
    void WriteUint32(uint32_t v) { WriteRaw(&v, sizeof(v), 4); }
    void WriteInt64(int64_t v)   { WriteRaw(&v, sizeof(v), 8); }
    void WriteUint64(uint64_t v) { WriteRaw(&v, sizeof(v), 8); }
    void WriteFloat(float v)     { WriteRaw(&v, sizeof(v), 4); }
    void WriteDouble(double v)   { WriteRaw(&v, sizeof(v), 8); }
    void WriteBool(bool v)       { uint8_t b = v ? 1 : 0; WriteRaw(&b, 1, 1); }

    /*
     * Write a HIDL hidl_string into both buffers:
     *   - In `sg_`: a 16-byte HidlString struct {buffer,size,owns,pad}
     *     followed by the string bytes (NUL-terminated, 8-byte padded).
     *   - In `data_`: two binder_buffer_objects.  First wraps the
     *     HidlString struct (no parent).  Second wraps the string bytes
     *     with parent=<first object's offset index> and parent_offset=0
     *     (= offset of HidlString::buffer inside the struct).
     *
     * Kernel quirk: binder_validate_fixup() rejects the transaction if
     * the *first* binder object in `data_` sits at data offset 0.
     * Callers MUST write some non-PTR prefix (e.g. an interface token)
     * before the first hidl_string.
     */
    void WriteHidlString(const std::string &s);

    /* ─── Accessors — for HwBinderClient::Transact() ────────────────── */

    const uint8_t *Data() const     { return data_.data(); }
    size_t DataSize() const          { return data_.size(); }
    const uint64_t *Offsets() const { return offsets_.data(); }
    size_t OffsetsSize() const       { return offsets_.size() * sizeof(uint64_t); }
    /*
     * Scatter-gather buffer size the kernel must allocate next to the
     * main buffer (sum of all embedded buffer lengths, individually
     * 8-byte aligned).
     */
    size_t SgSize() const            { return sgSize_; }

    /* ─── Reader ─────────────────────────────────────────────────── */

    /*
     * Read a HIDL reply.  Constructed by HwBinderClient from a BR_REPLY's
     * binder_transaction_data.  Holds a const view; valid only until
     * HwBinderClient frees the kernel-allocated reply buffer.
     */
    class Reader {
    public:
        Reader(const void *data, size_t dataSize,
               const void *offsets, size_t offsetsSize, uint32_t flags)
            : data_(static_cast<const uint8_t *>(data)), dataSize_(dataSize),
              offsets_(static_cast<const uint64_t *>(offsets)),
              offsetsCount_(offsetsSize / sizeof(uint64_t)),
              flags_(flags) {}

        /*
         * True if the kernel/library returned a bare status_t (e.g. on
         * BR_REPLY-of-failure).  False for a normal HIDL Parcel reply
         * whose first int32 is the HIDL status (0 = EX_NONE).
         */
        bool IsStatus() const   { return flags_ & TF_STATUS_CODE; }
        uint32_t Flags() const  { return flags_; }
        size_t DataSize() const { return dataSize_; }

        bool ReadInt32(int32_t *out);
        bool ReadUint32(uint32_t *out);
        bool ReadInt64(int64_t *out);
        bool ReadUint64(uint64_t *out);

        /*
         * Read a hidl_string.  Returns the embedded string by value
         * (copy).  Advances past the inline HidlString descriptor; the
         * kernel has already fixed up the buffer pointer to live in our
         * mmap'd SG region.
         */
        bool ReadHidlString(std::string *out);

        /*
         * Read a flat_binder_object at the current cursor (8-byte
         * aligned).  Returns the contained handle (after the kernel
         * has rewritten BINDER_TYPE_BINDER → BINDER_TYPE_HANDLE) and
         * `isNull` if the object is null.  Does NOT acquire — caller
         * must `client.Acquire(handle)` before using.
         */
        bool ReadFlatBinder(uint32_t *outHandle, bool *isNull);

        /*
         * Read a hidl_vec<hidl_string> at the current cursor.  Uses
         * the kernel-fixed-up SG pointers inside the buffer_objects
         * to materialise each string into the output vector.  The
         * underlying Reply (owning the mmap'd region) must still be
         * alive — the strings reference SG memory; we copy them out
         * into std::string instances so the result outlives Reply.
         */
        bool ReadHidlStringVec(std::vector<std::string> *out);

        /* Skip n raw bytes; used by the (rare) caller that knows the
         * wire shape and just wants to seek past padding. */
        bool Skip(size_t n);

    private:
        const uint8_t  *data_;
        size_t          dataSize_;
        [[maybe_unused]] const uint64_t *offsets_;     /* used by future hidl_vec reader */
        [[maybe_unused]] size_t          offsetsCount_;
        uint32_t        flags_;
        size_t          cursor_ = 0;
    };

private:
    void WriteRaw(const void *p, size_t n, size_t alignTo);
    void AlignTo(size_t alignment);
    /* Append a binder_buffer_object to data_; returns its offsets index. */
    size_t AppendBufferObject(const binder_buffer_object &bo);
    /* Reserve `n` bytes of SG room (just bumps sgSize_; the source
     * memory lives in ownedBlobs_). */
    void GrowSg(size_t n);

    std::vector<uint8_t>  data_;
    std::vector<uint64_t> offsets_;
    size_t                sgSize_ = 0;
    /* Backing memory for SG buffer contents (HidlString structs, string
     * bytes, …).  Pointers in our binder_buffer_objects refer in here. */
    std::vector<std::unique_ptr<std::vector<uint8_t>>> ownedBlobs_;
};

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_PARCEL_H
