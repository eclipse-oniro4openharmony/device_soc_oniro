/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * HwBinderClient — pure-musl client for /dev/hwbinder (and friends).
 * Wraps open + BINDER_VERSION + mmap + BINDER_SET_MAX_THREADS +
 * BC_ENTER_LOOPER on construction, and exposes a synchronous
 * Transact(handle, code, request, &reply) that hides the BWR loop.
 *
 * No dependency on Android libhwbinder, libhidlbase, libutils, or any
 * bionic-compiled code.  Sits on the kernel binder ABI directly.
 */

#ifndef HYBRIS_HIDL_HW_BINDER_CLIENT_H
#define HYBRIS_HIDL_HW_BINDER_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>

#include "hw_binder_abi.h"
#include "hw_parcel.h"

namespace OHOS::Camera::Hybris::Hidl {

class HwBinderClient {
public:
    enum class Result {
        Ok,
        DriverOpenFailed,
        BadProtocolVersion,
        IoctlFailed,
        MmapFailed,
        DeadReply,
        FailedReply,
        BinderError,
        Truncated,
    };

    static const char *ResultName(Result r);

    /*
     * Open the requested binder device (default /dev/hwbinder), validate
     * protocol version 8, mmap a reply window, and BC_ENTER_LOOPER.
     * Returns Ok on success.
     */
    Result Open(const char *device = "/dev/hwbinder");
    ~HwBinderClient();

    /* RAII per-handle strong ref — BC_ACQUIRE on construction,
     * BC_RELEASE on destruction. */
    class StrongHandle {
    public:
        StrongHandle() = default;
        StrongHandle(HwBinderClient *c, uint32_t h) : client_(c), handle_(h) {}
        StrongHandle(StrongHandle &&o) noexcept
            : client_(o.client_), handle_(o.handle_), valid_(o.valid_)
        { o.valid_ = false; }
        StrongHandle &operator=(StrongHandle &&o) noexcept {
            if (this != &o) {
                Release();
                client_ = o.client_;
                handle_ = o.handle_;
                valid_ = o.valid_;
                o.valid_ = false;
            }
            return *this;
        }
        StrongHandle(const StrongHandle &) = delete;
        StrongHandle &operator=(const StrongHandle &) = delete;
        ~StrongHandle() { Release(); }

        uint32_t Handle() const { return handle_; }
        bool Valid() const { return valid_; }

    private:
        friend class HwBinderClient;
        void Release();
        HwBinderClient *client_ = nullptr;
        uint32_t handle_ = 0;
        bool valid_ = false;
    };

    /*
     * Take a strong ref on `handle`.  Returns an invalid StrongHandle
     * on failure (ioctl errno already logged).
     */
    StrongHandle Acquire(uint32_t handle);

    /*
     * Synchronous transact.  On success, `reply` is populated with
     * pointers into the kernel-allocated reply buffer in our mmap'd
     * region.  The buffer stays mapped (and pointers within it stay
     * valid) until `reply` is destroyed/moved-from — at which point
     * BC_FREE_BUFFER is sent automatically.
     *
     * This deferred-free behavior is needed for replies that embed
     * pointer fields (hidl_string, hidl_vec): the kernel fixes those
     * up to point inside our mmap'd SG region, so dereferencing them
     * requires the buffer to still be mapped.  An eager copy-and-free
     * approach would dangle those pointers.
     *
     * Reply flags: TF_STATUS_CODE carries a bare status_t (e.g.
     * enforceInterface failure = INT32_MIN+1).  flags=0 means a normal
     * HIDL Parcel reply whose first int32 is the HIDL exception code
     * (0 = EX_NONE).
     */
    struct Reply {
        const uint8_t  *data        = nullptr;
        size_t          dataSize    = 0;
        const uint64_t *offsets     = nullptr;
        size_t          offsetsSize = 0;  /* in bytes */
        uint32_t        flags       = 0;

        Reply() = default;
        Reply(const Reply &) = delete;
        Reply &operator=(const Reply &) = delete;
        Reply(Reply &&o) noexcept;
        Reply &operator=(Reply &&o) noexcept;
        ~Reply();

        HwParcel::Reader Reader() const {
            return HwParcel::Reader(data, dataSize,
                                    offsets, offsetsSize, flags);
        }

    private:
        friend class HwBinderClient;
        int    fd_  = -1;   /* /dev/hwbinder fd for BC_FREE_BUFFER */
        void  *raw_ = nullptr;  /* arg to BC_FREE_BUFFER (= data) */
    };

    Result Transact(uint32_t handle, uint32_t code,
                    const HwParcel &request, Reply *reply,
                    uint32_t txFlags = TF_ACCEPT_FDS);

    int Fd() const { return fd_; }

private:
    int    fd_ = -1;
    void  *mapped_ = nullptr;
    size_t mapSize_ = 256 * 1024;

    bool SendCommand(const void *buf, size_t len);
    bool EnterLooper();
    Result ParseReplies(const std::vector<uint8_t> &readBuf,
                        size_t consumed, Reply *reply);
};

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_BINDER_CLIENT_H
