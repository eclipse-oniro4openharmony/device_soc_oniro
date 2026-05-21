/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * HwBinderServer — minimal server-side binder dispatch loop.
 *
 * When we call into Halium via `ICameraDevice::open(callback)` the
 * callback argument is an `sp<ICameraDeviceCallback>` — Halium expects
 * to BC_TRANSACTION back into our process for `processCaptureResult`,
 * `notify`, etc.  For that to work we need (a) to expose a binder node
 * by sending a `BINDER_TYPE_BINDER` flat_binder_object in the request,
 * and (b) a worker thread in our process that reads BR_TRANSACTION
 * commands off /dev/hwbinder and dispatches them.
 *
 * This class is that worker thread + the dispatch table.  It runs
 * alongside the existing `HwBinderClient` (single shared
 * `/dev/hwbinder` fd is fine — the binder driver multiplexes commands
 * to the right thread automatically).
 *
 * Lifecycle:
 *   - Construct: takes a shared fd (the one HwBinderClient owns).
 *   - Register one or more `LocalBinder`s — each is identified by a
 *     pointer (the `binder` and `cookie` fields of the
 *     `flat_binder_object` we send in the request).  Owner of the
 *     LocalBinder must outlive the server.
 *   - Start(): spawn worker thread, send `BC_REGISTER_LOOPER`, enter
 *     the BWR loop.  Pumps reads of BR_* commands and dispatches
 *     BR_TRANSACTION to the matching LocalBinder::OnTransact.
 *   - Stop(): join the worker.
 *
 * Thread safety: registrations done before Start(); not modified after.
 */

#ifndef HYBRIS_HIDL_HW_BINDER_SERVER_H
#define HYBRIS_HIDL_HW_BINDER_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "hw_binder_abi.h"
#include "hw_parcel.h"

namespace OHOS::Camera::Hybris::Hidl {

class LocalBinder {
public:
    /*
     * Invoked from a server worker thread when a remote calls into
     * this binder.  `data` is a Reader over the request parcel
     * (already enforceInterface-clean; the implementation MUST call
     * Reader and read the interface descriptor itself).  `reply` is
     * an HwParcel the impl populates with the response payload.
     *
     * Return value: HIDL EX_NONE = 0 on success, non-zero on error.
     */
    virtual ~LocalBinder() = default;
    virtual int32_t OnTransact(uint32_t code, HwParcel::Reader &data,
                               HwParcel *reply) = 0;

    /*
     * Each LocalBinder is identified to the kernel by an opaque
     * pointer pair (binder, cookie).  We use the LocalBinder*
     * itself for both fields — unique per object.
     */
    uintptr_t Key() const { return reinterpret_cast<uintptr_t>(this); }
};

class HwBinderServer {
public:
    explicit HwBinderServer(int fd) : fd_(fd) {}
    ~HwBinderServer();

    HwBinderServer(const HwBinderServer &) = delete;
    HwBinderServer &operator=(const HwBinderServer &) = delete;

    /*
     * Register a local binder.  Must be called before Start().
     * The HwBinderServer does NOT take ownership — caller keeps
     * the LocalBinder alive for the lifetime of the server.
     */
    void Register(LocalBinder *binder);

    /*
     * Spawn the worker thread.  Sends BC_ENTER_LOOPER then enters
     * the BWR loop.  Returns true on success.
     */
    bool Start();

    /* Signal the worker thread to exit and join. */
    void Stop();

    /*
     * Free a kernel-allocated request buffer.  Worker thread does
     * this automatically after dispatching, but exposed here for
     * tests.
     */
    void FreeBuffer(uintptr_t bufferPtr);

    /*
     * Dispatch an inline BR_TRANSACTION that arrived on a client
     * thread (nested call: the remote called back into us while
     * we were waiting for our own BR_REPLY).  Same handler as the
     * worker-loop dispatch path; safe to call from any thread
     * sharing the same /dev/hwbinder fd.
     */
    void DispatchTransaction(const binder_transaction_data &td);

private:
    void WorkerLoop();
    bool SendCommand(const void *cmd, size_t n);

    int                                fd_;
    std::atomic<bool>                  running_{false};
    std::thread                        worker_;
    std::map<uintptr_t, LocalBinder *> binders_;
};

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_BINDER_SERVER_H
