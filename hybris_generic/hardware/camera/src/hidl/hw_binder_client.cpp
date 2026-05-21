/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hw_binder_client.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

#include "hw_binder_server.h"
#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris::Hidl {

const char *HwBinderClient::ResultName(Result r)
{
    switch (r) {
        case Result::Ok:                 return "Ok";
        case Result::DriverOpenFailed:   return "DriverOpenFailed";
        case Result::BadProtocolVersion: return "BadProtocolVersion";
        case Result::IoctlFailed:        return "IoctlFailed";
        case Result::MmapFailed:         return "MmapFailed";
        case Result::DeadReply:          return "DeadReply";
        case Result::FailedReply:        return "FailedReply";
        case Result::BinderError:        return "BinderError";
        case Result::Truncated:          return "Truncated";
    }
    return "?";
}

HwBinderClient::~HwBinderClient()
{
    if (mapped_) munmap(mapped_, mapSize_);
    if (fd_ >= 0) close(fd_);
}

HwBinderClient::Result HwBinderClient::Open(const char *device)
{
    fd_ = open(device, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        CAMERA_VDI_LOGE("open(%{public}s): errno=%{public}d (%{public}s)",
                        device, errno, strerror(errno));
        return Result::DriverOpenFailed;
    }
    binder_version v = {0};
    if (ioctl(fd_, BINDER_VERSION, &v) < 0) {
        CAMERA_VDI_LOGE("BINDER_VERSION: errno=%{public}d", errno);
        return Result::IoctlFailed;
    }
    if (v.protocol_version != 8) {
        CAMERA_VDI_LOGE("binder protocol %{public}d, expected 8",
                        v.protocol_version);
        return Result::BadProtocolVersion;
    }
    uint32_t maxThreads = 1;
    if (ioctl(fd_, BINDER_SET_MAX_THREADS, &maxThreads) < 0) {
        CAMERA_VDI_LOGE("BINDER_SET_MAX_THREADS: errno=%{public}d", errno);
        return Result::IoctlFailed;
    }
    mapped_ = mmap(nullptr, mapSize_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped_ == MAP_FAILED) {
        CAMERA_VDI_LOGE("mmap: errno=%{public}d", errno);
        mapped_ = nullptr;
        return Result::MmapFailed;
    }
    if (!EnterLooper()) {
        return Result::IoctlFailed;
    }
    return Result::Ok;
}

bool HwBinderClient::SendCommand(const void *buf, size_t len)
{
    binder_write_read bwr = {};
    bwr.write_size = len;
    bwr.write_buffer = reinterpret_cast<uintptr_t>(buf);
    if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
        CAMERA_VDI_LOGE("BINDER_WRITE_READ (no-read): errno=%{public}d",
                        errno);
        return false;
    }
    return true;
}

bool HwBinderClient::EnterLooper()
{
    uint32_t cmd = BC_ENTER_LOOPER;
    return SendCommand(&cmd, sizeof(cmd));
}

HwBinderClient::StrongHandle HwBinderClient::Acquire(uint32_t handle)
{
    uint32_t buf[2] = { (uint32_t)BC_ACQUIRE, handle };
    if (!SendCommand(buf, sizeof(buf))) {
        return StrongHandle{};
    }
    StrongHandle h(this, handle);
    h.valid_ = true;
    return h;
}

void HwBinderClient::StrongHandle::Release()
{
    if (!valid_ || !client_) return;
    uint32_t buf[2] = { (uint32_t)BC_RELEASE, handle_ };
    client_->SendCommand(buf, sizeof(buf));
    valid_ = false;
}

HwBinderClient::Result HwBinderClient::Transact(uint32_t handle,
                                                uint32_t code,
                                                const HwParcel &request,
                                                Reply *reply,
                                                uint32_t txFlags)
{
    binder_transaction_data_sg sg = {};
    sg.transaction_data.target.handle = handle;
    sg.transaction_data.code  = code;
    sg.transaction_data.flags = txFlags;
    sg.transaction_data.data_size    = request.DataSize();
    sg.transaction_data.offsets_size = request.OffsetsSize();
    sg.transaction_data.data.ptr.buffer  =
        reinterpret_cast<uintptr_t>(request.Data());
    sg.transaction_data.data.ptr.offsets =
        reinterpret_cast<uintptr_t>(request.Offsets());
    sg.buffers_size = request.SgSize();

    std::vector<uint8_t> writeBuf(sizeof(uint32_t) + sizeof(sg));
    uint32_t cmd = BC_TRANSACTION_SG;
    memcpy(writeBuf.data(), &cmd, sizeof(cmd));
    memcpy(writeBuf.data() + sizeof(cmd), &sg, sizeof(sg));

    std::vector<uint8_t> readBuf(8192);

    binder_write_read bwr = {};
    bwr.write_size   = writeBuf.size();
    bwr.write_buffer = reinterpret_cast<uintptr_t>(writeBuf.data());
    bwr.read_size    = readBuf.size();
    bwr.read_buffer  = reinterpret_cast<uintptr_t>(readBuf.data());

    if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
        CAMERA_VDI_LOGE("BINDER_WRITE_READ (transact): errno=%{public}d",
                        errno);
        return Result::IoctlFailed;
    }

    /*
     * Pump the read side until we see a terminal reply.  Mirrors
     * AOSP IPCThreadState::waitForResponse — the first BWR may
     * return only BR_NOOP + BR_TRANSACTION_COMPLETE if the remote
     * end takes a while to produce the actual BR_REPLY (e.g.
     * getCameraDeviceInterface_V3_x has to instantiate a new
     * BnHwCameraDevice on the HAL side).  Without this loop fast
     * RPCs work but anything that bounces through dlopen on the
     * far side comes back as Result::Truncated.
     */
    for (;;) {
        Result rc = ParseReplies(readBuf, bwr.read_consumed, reply);
        if (rc != Result::Truncated) {
            return rc;
        }
        binder_write_read bwr2 = {};
        bwr2.read_size   = readBuf.size();
        bwr2.read_buffer = reinterpret_cast<uintptr_t>(readBuf.data());
        if (ioctl(fd_, BINDER_WRITE_READ, &bwr2) < 0) {
            CAMERA_VDI_LOGE("BINDER_WRITE_READ (drain): errno=%{public}d",
                            errno);
            return Result::IoctlFailed;
        }
        if (bwr2.read_consumed == 0) {
            return Result::Truncated;
        }
        bwr.read_consumed = bwr2.read_consumed;
    }
}

HwBinderClient::Reply::Reply(Reply &&o) noexcept
    : data(o.data), dataSize(o.dataSize), offsets(o.offsets),
      offsetsSize(o.offsetsSize), flags(o.flags),
      fd_(o.fd_), raw_(o.raw_)
{
    o.data = nullptr;
    o.dataSize = 0;
    o.offsets = nullptr;
    o.offsetsSize = 0;
    o.flags = 0;
    o.fd_ = -1;
    o.raw_ = nullptr;
}

HwBinderClient::Reply &HwBinderClient::Reply::operator=(Reply &&o) noexcept
{
    if (this != &o) {
        this->~Reply();
        new (this) Reply(std::move(o));
    }
    return *this;
}

HwBinderClient::Reply::~Reply()
{
    if (fd_ < 0 || !raw_) return;
    struct __attribute__((packed)) {
        uint32_t cmd;
        uint64_t ptr;
    } freeBuf = { (uint32_t)BC_FREE_BUFFER,
                  reinterpret_cast<uint64_t>(raw_) };
    binder_write_read bwr = {};
    bwr.write_size = sizeof(freeBuf);
    bwr.write_buffer = reinterpret_cast<uintptr_t>(&freeBuf);
    if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
        CAMERA_VDI_LOGW("BC_FREE_BUFFER: errno=%{public}d", errno);
    }
    fd_ = -1;
    raw_ = nullptr;
}

HwBinderClient::Result HwBinderClient::ParseReplies(
    const std::vector<uint8_t> &readBuf, size_t consumed, Reply *reply)
{
    size_t off = 0;
    while (off + sizeof(uint32_t) <= consumed) {
        uint32_t rcmd;
        memcpy(&rcmd, readBuf.data() + off, sizeof(rcmd));
        off += sizeof(rcmd);

        switch (rcmd) {
            case (uint32_t)BR_NOOP:
            case (uint32_t)BR_TRANSACTION_COMPLETE:
            case (uint32_t)BR_SPAWN_LOOPER:
                continue;
            case (uint32_t)BR_INCREFS: {
                /*
                 * Halium acquired a reference to one of our local
                 * binder objects (the callback passed to
                 * ICameraDevice::open()).  Acknowledge by sending
                 * BC_INCREFS_DONE — without it the kernel keeps
                 * delivering BR_INCREFS and the open() reply never
                 * arrives.
                 */
                if (off + sizeof(binder_ptr_cookie) > consumed) {
                    return Result::Truncated;
                }
                binder_ptr_cookie pc;
                memcpy(&pc, readBuf.data() + off, sizeof(pc));
                off += sizeof(pc);
                struct __attribute__((packed)) {
                    uint32_t cmd;
                    binder_ptr_cookie pc;
                } done = { (uint32_t)BC_INCREFS_DONE, pc };
                binder_write_read bwrAck = {};
                bwrAck.write_size = sizeof(done);
                bwrAck.write_buffer = reinterpret_cast<uintptr_t>(&done);
                ioctl(fd_, BINDER_WRITE_READ, &bwrAck);
                continue;
            }
            case (uint32_t)BR_ACQUIRE: {
                if (off + sizeof(binder_ptr_cookie) > consumed) {
                    return Result::Truncated;
                }
                binder_ptr_cookie pc;
                memcpy(&pc, readBuf.data() + off, sizeof(pc));
                off += sizeof(pc);
                struct __attribute__((packed)) {
                    uint32_t cmd;
                    binder_ptr_cookie pc;
                } done = { (uint32_t)BC_ACQUIRE_DONE, pc };
                binder_write_read bwrAck = {};
                bwrAck.write_size = sizeof(done);
                bwrAck.write_buffer = reinterpret_cast<uintptr_t>(&done);
                ioctl(fd_, BINDER_WRITE_READ, &bwrAck);
                continue;
            }
            case (uint32_t)BR_RELEASE:
            case (uint32_t)BR_DECREFS:
                /* Halium dropped a ref.  No reply needed; just skip
                 * the binder_ptr_cookie payload. */
                if (off + sizeof(binder_ptr_cookie) > consumed) {
                    return Result::Truncated;
                }
                off += sizeof(binder_ptr_cookie);
                continue;
            case (uint32_t)BR_TRANSACTION: {
                /*
                 * Nested call: while the client thread is waiting
                 * for BR_REPLY from a Halium method (e.g.
                 * ICameraDevice::open()), Halium calls back into
                 * our process — kernel routes the incoming
                 * BR_TRANSACTION to the same thread to avoid
                 * deadlock.  Dispatch it to the registered server
                 * (the LocalBinder lookup + reply-build path).
                 */
                if (off + sizeof(binder_transaction_data) > consumed) {
                    return Result::Truncated;
                }
                binder_transaction_data td;
                memcpy(&td, readBuf.data() + off, sizeof(td));
                off += sizeof(td);
                if (server_ != nullptr) {
                    server_->DispatchTransaction(td);
                } else {
                    CAMERA_VDI_LOGE("BR_TRANSACTION with no server "
                                    "registered (cookie=0x%{public}lx)",
                                    (unsigned long)td.cookie);
                    struct __attribute__((packed)) {
                        uint32_t cmd;
                        uint64_t ptr;
                    } freeBuf = {
                        (uint32_t)BC_FREE_BUFFER, td.data.ptr.buffer
                    };
                    binder_write_read bwrFree = {};
                    bwrFree.write_size = sizeof(freeBuf);
                    bwrFree.write_buffer = reinterpret_cast<uintptr_t>(&freeBuf);
                    ioctl(fd_, BINDER_WRITE_READ, &bwrFree);
                }
                continue;
            }
            case (uint32_t)BR_DEAD_BINDER:
                if (off + sizeof(binder_uintptr_t) > consumed) {
                    return Result::Truncated;
                }
                off += sizeof(binder_uintptr_t);
                continue;
            case (uint32_t)BR_REPLY: {
                if (off + sizeof(binder_transaction_data) > consumed) {
                    return Result::Truncated;
                }
                binder_transaction_data td;
                memcpy(&td, readBuf.data() + off, sizeof(td));
                off += sizeof(td);

                /* Copy data + offsets out before freeing the kernel
                 * buffer.  HidlString::buffer pointers inside the data
                 * become dangling once we BC_FREE_BUFFER, so a follow-up
                 * Reader that walks pointers (none in our current cases)
                 * would need to dereference *before* the free.  Pure
                 * scalar replies are fine. */
                const uint8_t *bufData =
                    reinterpret_cast<const uint8_t *>(td.data.ptr.buffer);
                const uint64_t *bufOffsets =
                    reinterpret_cast<const uint64_t *>(td.data.ptr.offsets);
                size_t nObj = td.offsets_size / sizeof(uint64_t);

                /*
                 * BC_ACQUIRE any strong binder handles in the reply
                 * BEFORE the eventual BC_FREE_BUFFER.  The kernel
                 * binder_transaction_buffer_release decrements refs on
                 * each BINDER_TYPE_HANDLE during free — without our
                 * own acquire first, the handle goes to ref=0 and any
                 * subsequent BC_TRANSACTION to it returns
                 * BR_FAILED_REPLY ("to 0:0 ret 29201/-22" in
                 * /sys/kernel/debug/binder/failed_transaction_log).
                 *
                 * Mirrors AOSP IPCThreadState::executeCommand BR_REPLY
                 * path which incStrongHandle's every reply binder.
                 */
                for (size_t i = 0; i < nObj; ++i) {
                    if (bufOffsets[i] + sizeof(flat_binder_object) > td.data_size)
                        continue;
                    flat_binder_object obj;
                    memcpy(&obj, bufData + bufOffsets[i], sizeof(obj));
                    if (obj.hdr.type == (uint32_t)BINDER_TYPE_HANDLE) {
                        uint32_t acq[2] = { (uint32_t)BC_ACQUIRE, obj.handle };
                        binder_write_read bwr3 = {};
                        bwr3.write_size = sizeof(acq);
                        bwr3.write_buffer =
                            reinterpret_cast<uintptr_t>(acq);
                        if (ioctl(fd_, BINDER_WRITE_READ, &bwr3) < 0) {
                            CAMERA_VDI_LOGW(
                                "BC_ACQUIRE(reply handle=%{public}u): "
                                "errno=%{public}d", obj.handle, errno);
                        }
                    }
                }

                /*
                 * Populate Reply with pointers into the kernel-mapped
                 * reply buffer.  BC_FREE_BUFFER is deferred to ~Reply
                 * so that pointer fields inside the reply (hidl_string
                 * buffer, hidl_vec backing array, …) stay valid until
                 * the caller is done reading.
                 */
                if (reply) {
                    reply->data        = bufData;
                    reply->dataSize    = td.data_size;
                    reply->offsets     = bufOffsets;
                    reply->offsetsSize = td.offsets_size;
                    reply->flags       = td.flags;
                    reply->fd_         = fd_;
                    reply->raw_        = const_cast<uint8_t *>(bufData);
                } else {
                    /* Caller didn't ask for the reply — free immediately. */
                    struct __attribute__((packed)) {
                        uint32_t cmd;
                        uint64_t ptr;
                    } freeBuf = { (uint32_t)BC_FREE_BUFFER, td.data.ptr.buffer };
                    binder_write_read bwr2 = {};
                    bwr2.write_size = sizeof(freeBuf);
                    bwr2.write_buffer = reinterpret_cast<uintptr_t>(&freeBuf);
                    ioctl(fd_, BINDER_WRITE_READ, &bwr2);
                }
                return Result::Ok;
            }
            case (uint32_t)BR_DEAD_REPLY:
                return Result::DeadReply;
            case (uint32_t)BR_FAILED_REPLY:
                return Result::FailedReply;
            case (uint32_t)BR_ERROR: {
                int32_t err = 0;
                if (off + sizeof(err) <= consumed) {
                    memcpy(&err, readBuf.data() + off, sizeof(err));
                    off += sizeof(err);
                    CAMERA_VDI_LOGE("BR_ERROR err=%{public}d", err);
                }
                return Result::BinderError;
            }
            default:
                CAMERA_VDI_LOGE("unexpected BR cmd 0x%{public}08x", rcmd);
                return Result::BinderError;
        }
    }
    return Result::Truncated;
}

} // namespace OHOS::Camera::Hybris::Hidl
