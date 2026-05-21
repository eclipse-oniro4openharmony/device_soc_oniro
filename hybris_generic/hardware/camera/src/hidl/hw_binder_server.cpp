/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hw_binder_server.h"

#include <cerrno>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris::Hidl {

HwBinderServer::~HwBinderServer()
{
    Stop();
}

void HwBinderServer::Register(LocalBinder *binder)
{
    binders_[binder->Key()] = binder;
}

bool HwBinderServer::SendCommand(const void *cmd, size_t n)
{
    binder_write_read bwr = {};
    bwr.write_size   = n;
    bwr.write_buffer = reinterpret_cast<uintptr_t>(cmd);
    if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
        CAMERA_VDI_LOGW("HwBinderServer SendCommand: errno=%{public}d",
                        errno);
        return false;
    }
    return true;
}

void HwBinderServer::FreeBuffer(uintptr_t bufferPtr)
{
    struct __attribute__((packed)) {
        uint32_t cmd;
        uint64_t ptr;
    } freeBuf = { (uint32_t)BC_FREE_BUFFER, bufferPtr };
    SendCommand(&freeBuf, sizeof(freeBuf));
}

bool HwBinderServer::Start()
{
    if (running_.exchange(true)) {
        return true;
    }
    /*
     * Need to call BC_ENTER_LOOPER from the worker thread itself —
     * the kernel binds the looper state to the calling task.
     * Spawn first, register inside the worker.
     */
    worker_ = std::thread(&HwBinderServer::WorkerLoop, this);
    return true;
}

void HwBinderServer::Stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    /*
     * The worker is blocked in BINDER_WRITE_READ.  Send ourselves a
     * dummy transaction (handle 0, the context manager) so the
     * ioctl returns and the loop sees running_=false.  Simplest:
     * close the fd... but that's owned by HwBinderClient.  For
     * shutdown during destruction this is fine — the process is
     * exiting.  Otherwise, the thread will exit on its own when
     * a transaction arrives.  TODO: more graceful shutdown.
     */
    if (worker_.joinable()) {
        worker_.detach();   // accept best-effort shutdown for now
    }
}

void HwBinderServer::WorkerLoop()
{
    /* Register as a worker thread willing to accept transactions. */
    uint32_t enterCmd = BC_ENTER_LOOPER;
    if (!SendCommand(&enterCmd, sizeof(enterCmd))) {
        CAMERA_VDI_LOGE("HwBinderServer: BC_ENTER_LOOPER failed");
        return;
    }
    CAMERA_VDI_LOGI("HwBinderServer: worker loop entered (%{public}zu binders)",
                    binders_.size());

    std::vector<uint8_t> readBuf(8192);
    while (running_.load()) {
        binder_write_read bwr = {};
        bwr.read_size   = readBuf.size();
        bwr.read_buffer = reinterpret_cast<uintptr_t>(readBuf.data());
        if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
            if (errno == EINTR) continue;
            CAMERA_VDI_LOGE("HwBinderServer: BWR errno=%{public}d", errno);
            break;
        }

        size_t off = 0;
        while (off + sizeof(uint32_t) <= bwr.read_consumed) {
            uint32_t cmd;
            memcpy(&cmd, readBuf.data() + off, sizeof(cmd));
            off += sizeof(cmd);

            switch (cmd) {
                case (uint32_t)BR_NOOP:
                case (uint32_t)BR_TRANSACTION_COMPLETE:
                case (uint32_t)BR_OK:
                case (uint32_t)BR_SPAWN_LOOPER:
                    break;
                case (uint32_t)BR_INCREFS: {
                    /* Send BC_INCREFS_DONE for our local binder. */
                    if (off + sizeof(binder_ptr_cookie) > bwr.read_consumed) {
                        off = bwr.read_consumed;
                        break;
                    }
                    binder_ptr_cookie pc;
                    memcpy(&pc, readBuf.data() + off, sizeof(pc));
                    off += sizeof(pc);
                    struct __attribute__((packed)) {
                        uint32_t cmd;
                        binder_ptr_cookie pc;
                    } done = { (uint32_t)BC_INCREFS_DONE, pc };
                    SendCommand(&done, sizeof(done));
                    break;
                }
                case (uint32_t)BR_ACQUIRE: {
                    if (off + sizeof(binder_ptr_cookie) > bwr.read_consumed) {
                        off = bwr.read_consumed;
                        break;
                    }
                    binder_ptr_cookie pc;
                    memcpy(&pc, readBuf.data() + off, sizeof(pc));
                    off += sizeof(pc);
                    struct __attribute__((packed)) {
                        uint32_t cmd;
                        binder_ptr_cookie pc;
                    } done = { (uint32_t)BC_ACQUIRE_DONE, pc };
                    SendCommand(&done, sizeof(done));
                    break;
                }
                case (uint32_t)BR_RELEASE:
                case (uint32_t)BR_DECREFS:
                    /* Skip the binder_ptr_cookie payload. */
                    if (off + sizeof(binder_ptr_cookie) > bwr.read_consumed) {
                        off = bwr.read_consumed;
                    } else {
                        off += sizeof(binder_ptr_cookie);
                    }
                    break;
                case (uint32_t)BR_TRANSACTION: {
                    if (off + sizeof(binder_transaction_data) >
                        bwr.read_consumed) {
                        off = bwr.read_consumed;
                        break;
                    }
                    binder_transaction_data td;
                    memcpy(&td, readBuf.data() + off, sizeof(td));
                    off += sizeof(td);
                    DispatchTransaction(td);
                    break;
                }
                case (uint32_t)BR_DEAD_BINDER: {
                    if (off + sizeof(binder_uintptr_t) > bwr.read_consumed) {
                        off = bwr.read_consumed;
                    } else {
                        off += sizeof(binder_uintptr_t);
                    }
                    CAMERA_VDI_LOGW("HwBinderServer: BR_DEAD_BINDER");
                    break;
                }
                case (uint32_t)BR_FAILED_REPLY:
                    CAMERA_VDI_LOGW("HwBinderServer: BR_FAILED_REPLY");
                    break;
                case (uint32_t)BR_ERROR: {
                    int32_t err = 0;
                    if (off + sizeof(err) <= bwr.read_consumed) {
                        memcpy(&err, readBuf.data() + off, sizeof(err));
                        off += sizeof(err);
                    }
                    CAMERA_VDI_LOGE("HwBinderServer: BR_ERROR err=%{public}d",
                                    err);
                    break;
                }
                default:
                    CAMERA_VDI_LOGW("HwBinderServer: unexpected BR cmd "
                                    "0x%{public}08x", cmd);
                    off = bwr.read_consumed;
                    break;
            }
        }
    }
    CAMERA_VDI_LOGI("HwBinderServer: worker loop exited");
}

void HwBinderServer::DispatchTransaction(const binder_transaction_data &td)
{
    /*
     * Find the LocalBinder for this transaction.  Kernel passes
     * (target.ptr, cookie) — for our case we encode the LocalBinder*
     * in both fields when we send the BINDER_TYPE_BINDER object.
     */
    auto it = binders_.find(static_cast<uintptr_t>(td.cookie));
    if (it == binders_.end()) {
        CAMERA_VDI_LOGE("HwBinderServer: no LocalBinder for cookie "
                        "0x%{public}lx", static_cast<unsigned long>(td.cookie));
        /* Still need to free + send a reply or the remote times out. */
        FreeBuffer(td.data.ptr.buffer);
        HwParcel emptyReply;
        emptyReply.WriteInt32(-EINVAL);
        binder_transaction_data_sg replySg = {};
        replySg.transaction_data.flags     = TF_STATUS_CODE;
        replySg.transaction_data.data_size    = emptyReply.DataSize();
        replySg.transaction_data.offsets_size = 0;
        replySg.transaction_data.data.ptr.buffer  =
            reinterpret_cast<uintptr_t>(emptyReply.Data());
        replySg.transaction_data.data.ptr.offsets = 0;
        replySg.buffers_size = 0;
        struct __attribute__((packed)) {
            uint32_t cmd;
            binder_transaction_data_sg sg;
        } reply = { (uint32_t)BC_REPLY_SG, replySg };
        SendCommand(&reply, sizeof(reply));
        return;
    }

    LocalBinder *binder = it->second;
    HwParcel::Reader reader(
        reinterpret_cast<const void *>(td.data.ptr.buffer),
        td.data_size,
        reinterpret_cast<const void *>(td.data.ptr.offsets),
        td.offsets_size,
        td.flags);

    HwParcel replyParcel;
    int32_t status = binder->OnTransact(td.code, reader, &replyParcel);

    /* Done reading the request buffer — free it. */
    FreeBuffer(td.data.ptr.buffer);

    /*
     * For one-way (oneway) transactions we don't reply at all.
     * Otherwise build BC_REPLY_SG with the parcel.  For a HIDL
     * method the reply parcel is the populated response; for a
     * status-only reply we set TF_STATUS_CODE with status in
     * data.buf.
     */
    if (td.flags & TF_ONE_WAY) {
        return;
    }

    binder_transaction_data_sg replySg = {};
    replySg.transaction_data.flags     = 0;
    if (status != 0) {
        /* Bare-status reply.  HwParcel content is ignored. */
        replySg.transaction_data.flags        = TF_STATUS_CODE;
        replySg.transaction_data.data_size    = sizeof(status);
        replySg.transaction_data.offsets_size = 0;
        memcpy(replySg.transaction_data.data.buf, &status, sizeof(status));
    } else {
        replySg.transaction_data.data_size    = replyParcel.DataSize();
        replySg.transaction_data.offsets_size = replyParcel.OffsetsSize();
        replySg.transaction_data.data.ptr.buffer =
            reinterpret_cast<uintptr_t>(replyParcel.Data());
        replySg.transaction_data.data.ptr.offsets =
            reinterpret_cast<uintptr_t>(replyParcel.Offsets());
        replySg.buffers_size = replyParcel.SgSize();
    }

    struct __attribute__((packed)) {
        uint32_t cmd;
        binder_transaction_data_sg sg;
    } reply = { (uint32_t)BC_REPLY_SG, replySg };
    SendCommand(&reply, sizeof(reply));
}

} // namespace OHOS::Camera::Hybris::Hidl
