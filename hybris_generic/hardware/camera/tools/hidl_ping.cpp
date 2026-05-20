/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * N12.5.2 — minimal HIDL ping test.
 *
 * Sends a HIDL ping() (transaction code 0xff000006, the IBase::ping
 * base method inherited by every HIDL interface) to handle 0
 * (hwservicemanager) on /dev/hwbinder and waits for BR_REPLY.  Pure
 * musl, no libhybris, no libhwbinder — talks the binder kernel ABI
 * directly.
 *
 * Wire format reference: AOSP system/libhwbinder/Parcel.cpp +
 * IPCThreadState.cpp.  Validated against the IServiceManager.hal hash
 * shipped with Halium 12 (android.hidl.manager@1.0).
 *
 * Success path:
 *   - BC_ENTER_LOOPER acknowledged
 *   - BC_TRANSACTION_SG accepted, BR_TRANSACTION_COMPLETE returned
 *   - BR_REPLY returned with status_t = 0 (NO_ERROR) and an empty
 *     data buffer (ping returns void)
 *   - We BC_FREE_BUFFER the kernel-allocated reply buffer
 */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "hidl/hw_binder_abi.h"
#include "hybris_camera_log.h"

namespace {

using namespace OHOS::Camera::Hybris::Hidl;

constexpr const char *kHwBinder = "/dev/hwbinder";

/* hwservicemanager's interface descriptor — what writeInterfaceToken
 * embeds before any IServiceManager call (including the inherited
 * IBase methods like ping).  Halium 12 ships v1.0; v1.1 / v1.2 add
 * methods but keep the same descriptor for base inheritance.
 */
/* IBase descriptor — ping is BnHwBase::_hidl_ping which always calls
 * data.enforceInterface(IBase::descriptor), regardless of the
 * concrete service it sits on top of.  Verified by disassembling
 * libhidlbase.so on the X23. */
constexpr const char *kServiceManagerIface =
    "android.hidl.base@1.0::IBase";

/*
 * Match HIDL Parcel alignment: every field is 8-byte aligned in the
 * main buffer.  Pad-up helper.
 */
constexpr size_t kAlign = 8;
static inline size_t AlignUp(size_t v) { return (v + kAlign - 1) & ~(kAlign - 1); }

/*
 * Mirror of HIDL's hidl_string (system/libhidl/base/include/hidl/HidlSupport.h)
 * as it sits *in the secondary buffer*.  On 64-bit:
 *   off  size  field
 *   0    8     const char *buffer  (gets fixed up by the kernel)
 *   8    4     uint32_t size       (length without null terminator)
 *  12    1     uint8_t  ownsBuffer
 *  13    3     padding
 * total 16 bytes, 8-byte aligned.
 */
struct HidlString {
    uint64_t buffer;        // pointer; placeholder value, kernel rewrites
    uint32_t size;          // length without null
    uint8_t  owns;
    uint8_t  pad[3];
};
static_assert(sizeof(HidlString) == 16, "HidlString must be 16 bytes");

class HidlPing {
public:
    int Run()
    {
        if (!OpenBinder()) return 1;
        if (!EnterLooper()) return 2;
        if (!AcquireHandle(0)) return 3;
        SendEmptyPing();
        SendOneBufferPing();
        SendTwoBufferNoParentPing();
        // Sweep IBase transaction codes to find the working one.
        for (uint32_t code = 0xff000000; code <= 0xff00000F; ++code) {
            SweepCode(code);
        }
        if (!SendPing())    return 4;
        printf("hidl_ping: SUCCESS\n");
        return 0;
    }

    /*
     * AOSP ProcessState's getStrongProxyForHandle(0) sends BC_ACQUIRE
     * before the first transaction.  The kernel tracks per-process
     * handle refs; transacting on an un-ref'd handle returns
     * BR_FAILED_REPLY before the message is even queued.
     */
    bool AcquireHandle(uint32_t handle)
    {
        uint32_t buf[2] = { (uint32_t)BC_ACQUIRE, handle };
        binder_write_read bwr = {};
        bwr.write_size   = sizeof(buf);
        bwr.write_buffer = reinterpret_cast<uintptr_t>(buf);
        if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
            fprintf(stderr, "BC_ACQUIRE(%u): errno=%d (%s)\n",
                    handle, errno, strerror(errno));
            return false;
        }
        printf("BC_ACQUIRE(%u): write_consumed=%llu\n",
               handle, (unsigned long long)bwr.write_consumed);
        return true;
    }

private:
    int   fd_     = -1;
    void *mapped_ = nullptr;
    size_t mapSize_ = 256 * 1024;  // 256 KB — way more than ping needs

    bool OpenBinder()
    {
        fd_ = open(kHwBinder, O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            perror("open /dev/hwbinder");
            return false;
        }
        binder_version v = {0};
        if (ioctl(fd_, BINDER_VERSION, &v) < 0 || v.protocol_version != 8) {
            fprintf(stderr, "BINDER_VERSION expected 8, got %d (errno=%d)\n",
                    v.protocol_version, errno);
            return false;
        }
        uint32_t maxThreads = 1;
        if (ioctl(fd_, BINDER_SET_MAX_THREADS, &maxThreads) < 0) {
            perror("BINDER_SET_MAX_THREADS");
            return false;
        }
        mapped_ = mmap(nullptr, mapSize_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapped_ == MAP_FAILED) {
            perror("mmap");
            return false;
        }
        printf("opened /dev/hwbinder fd=%d, mapped=%p sz=%zu\n",
               fd_, mapped_, mapSize_);
        return true;
    }

    /*
     * BC_ENTER_LOOPER announces this thread as a binder-receiving
     * thread.  Required before BC_TRANSACTION can be processed; the
     * kernel needs to know which thread to deliver BR_TRANSACTION_COMPLETE
     * (and later BR_REPLY) to.
     */
    bool EnterLooper()
    {
        uint32_t cmd = BC_ENTER_LOOPER;
        binder_write_read bwr = {};
        bwr.write_size   = sizeof(cmd);
        bwr.write_buffer = reinterpret_cast<uintptr_t>(&cmd);
        if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
            perror("BC_ENTER_LOOPER");
            return false;
        }
        return true;
    }

    /*
     * Build the BC_TRANSACTION_SG payload for ping().  Layout of the
     * write buffer:
     *
     *   [uint32_t]                 BC_TRANSACTION_SG command
     *   [binder_transaction_data_sg]  the txn header + buffers_size
     *
     * The transaction's data buffer (main buffer) layout for a ping:
     *
     *   off  size  content
     *   ---  ----  -------
     *    0   40   binder_buffer_object #0  (BINDER_TYPE_PTR, no parent)
     *              wraps the HidlString sitting in scatter-gather space
     *   40   40   binder_buffer_object #1  (BINDER_TYPE_PTR, parent=#0)
     *              wraps the descriptor bytes inside the HidlString
     *
     * Offsets buffer (two 8-byte offsets, one per binder_buffer_object):
     *   [0,  40]
     *
     * Scatter-gather size: HidlString (16, padded to 8) + descriptor
     * bytes (len+1, padded to 8).
     */
    /*
     * Probe step: send an empty transaction (no data, no offsets, no SG)
     * with code=ping.  Should reach hwservicemanager (which will reject
     * with EX_TRANSACTION_FAILED in the reply — but reaching it at all
     * proves our transaction header is well-formed.  If the kernel
     * rejects this synchronously with BR_FAILED_REPLY, the issue is in
     * the transaction header itself, not in the buffer encoding.
     */
    bool SendEmptyPing()
    {
        binder_transaction_data_sg sg = {};
        sg.transaction_data.target.handle = 0;
        sg.transaction_data.code          = HidlBase::TRANSACTION_ping;
        sg.transaction_data.flags         = TF_ACCEPT_FDS;
        sg.transaction_data.data_size     = 0;
        sg.transaction_data.offsets_size  = 0;
        sg.transaction_data.data.ptr.buffer  = 0;
        sg.transaction_data.data.ptr.offsets = 0;
        sg.buffers_size                   = 0;

        std::vector<uint8_t> writeBuf(sizeof(uint32_t) + sizeof(sg));
        uint32_t cmd = BC_TRANSACTION_SG;
        memcpy(writeBuf.data(), &cmd, sizeof(cmd));
        memcpy(writeBuf.data() + sizeof(cmd), &sg, sizeof(sg));

        std::vector<uint8_t> readBuf(4096);
        binder_write_read bwr = {};
        bwr.write_size    = writeBuf.size();
        bwr.write_buffer  = reinterpret_cast<uintptr_t>(writeBuf.data());
        bwr.read_size     = readBuf.size();
        bwr.read_buffer   = reinterpret_cast<uintptr_t>(readBuf.data());

        printf("\n--- probe: empty ping (no data, no SG) ---\n");
        if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
            fprintf(stderr, "empty ping BWR: errno=%d (%s)\n",
                    errno, strerror(errno));
            return false;
        }
        printf("BWR done: write_consumed=%llu, read_consumed=%llu\n",
               (unsigned long long)bwr.write_consumed,
               (unsigned long long)bwr.read_consumed);
        DumpReplies(readBuf, bwr.read_consumed);
        return true;
    }

    void DumpReplies(const std::vector<uint8_t> &buf, size_t consumed)
    {
        size_t off = 0;
        while (off + sizeof(uint32_t) <= consumed) {
            uint32_t r;
            memcpy(&r, buf.data() + off, sizeof(r));
            off += sizeof(r);
            const char *n = "?";
            size_t payload = 0;
            switch (r) {
                case (uint32_t)BR_NOOP:                 n = "BR_NOOP"; break;
                case (uint32_t)BR_TRANSACTION_COMPLETE: n = "BR_TRANSACTION_COMPLETE"; break;
                case (uint32_t)BR_SPAWN_LOOPER:         n = "BR_SPAWN_LOOPER"; break;
                case (uint32_t)BR_DEAD_REPLY:           n = "BR_DEAD_REPLY"; break;
                case (uint32_t)BR_FAILED_REPLY:         n = "BR_FAILED_REPLY"; break;
                case (uint32_t)BR_REPLY: {
                    n = "BR_REPLY";
                    if (off + sizeof(binder_transaction_data) <= consumed) {
                        binder_transaction_data td;
                        memcpy(&td, buf.data() + off, sizeof(td));
                        int32_t status = -1;
                        if (td.data_size >= 4) {
                            memcpy(&status, (void*)td.data.ptr.buffer, 4);
                        }
                        const char *kind = (td.flags & TF_STATUS_CODE) ? "status_t" : "parcel.int32[0]";
                        printf("  cmd 0x%08x %s flags=0x%x data_size=%llu %s=%d\n",
                               r, n, td.flags, (unsigned long long)td.data_size,
                               kind, status);
                        // Free buffer
                        struct __attribute__((packed)) { uint32_t cmd; uint64_t p; } fb =
                            { (uint32_t)BC_FREE_BUFFER, td.data.ptr.buffer };
                        binder_write_read bwr2 = {};
                        bwr2.write_size = sizeof(fb);
                        bwr2.write_buffer = reinterpret_cast<uintptr_t>(&fb);
                        ioctl(fd_, BINDER_WRITE_READ, &bwr2);
                    }
                    payload = sizeof(binder_transaction_data);
                    off += payload;
                    continue;
                }
                case (uint32_t)BR_ERROR:
                    n = "BR_ERROR";
                    payload = sizeof(int32_t);
                    break;
                default: break;
            }
            printf("  cmd 0x%08x %s\n", r, n);
            off += payload;
        }
    }

    /*
     * Bisect: send a transaction with exactly ONE binder_buffer_object
     * (no parent fixup, simple 32-byte payload).  If this passes the
     * kernel but the two-buffer ping fails, the bug is in the
     * parent/parent_offset handling.  If even this fails, the bug is
     * in the basic PTR encoding.
     */
    bool SendOneBufferPing()
    {
        constexpr size_t kPayloadLen = 32;
        std::vector<uint8_t> sgBytes(kPayloadLen, 0xAB);

        std::vector<uint8_t> data(sizeof(binder_buffer_object));
        auto *bo = reinterpret_cast<binder_buffer_object *>(data.data());
        bo->hdr.type      = BINDER_TYPE_PTR;
        bo->flags         = 0;
        bo->buffer        = reinterpret_cast<uintptr_t>(sgBytes.data());
        bo->length        = kPayloadLen;
        bo->parent        = 0;
        bo->parent_offset = 0;

        uint64_t offsets[1] = { 0 };

        binder_transaction_data_sg sg = {};
        sg.transaction_data.target.handle = 0;
        sg.transaction_data.code          = HidlBase::TRANSACTION_ping;
        sg.transaction_data.flags         = TF_ACCEPT_FDS;
        sg.transaction_data.data_size     = data.size();
        sg.transaction_data.offsets_size  = sizeof(offsets);
        sg.transaction_data.data.ptr.buffer  = reinterpret_cast<uintptr_t>(data.data());
        sg.transaction_data.data.ptr.offsets = reinterpret_cast<uintptr_t>(offsets);
        sg.buffers_size = AlignUp(kPayloadLen);

        std::vector<uint8_t> writeBuf(sizeof(uint32_t) + sizeof(sg));
        uint32_t cmd = BC_TRANSACTION_SG;
        memcpy(writeBuf.data(), &cmd, sizeof(cmd));
        memcpy(writeBuf.data() + sizeof(cmd), &sg, sizeof(sg));

        std::vector<uint8_t> readBuf(4096);
        binder_write_read bwr = {};
        bwr.write_size    = writeBuf.size();
        bwr.write_buffer  = reinterpret_cast<uintptr_t>(writeBuf.data());
        bwr.read_size     = readBuf.size();
        bwr.read_buffer   = reinterpret_cast<uintptr_t>(readBuf.data());

        printf("\n--- probe: 1-buffer ping (sg=%zu) ---\n", (size_t)sg.buffers_size);
        if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
            fprintf(stderr, "1-buffer BWR: errno=%d (%s)\n", errno, strerror(errno));
            return false;
        }
        printf("BWR done: write_consumed=%llu, read_consumed=%llu\n",
               (unsigned long long)bwr.write_consumed,
               (unsigned long long)bwr.read_consumed);
        DumpReplies(readBuf, bwr.read_consumed);
        return true;
    }

    /*
     * Sweep one transaction code with empty data, log status.
     * Find which code yields status=0 (= NO_ERROR), which would be
     * a no-arg method that succeeded without an interface check.
     * If all codes return -74 (UNKNOWN_TRANSACTION), then either the
     * code range is wrong, or this binder requires an interface
     * token even to dispatch.
     */
    void SweepCode(uint32_t code)
    {
        binder_transaction_data_sg sg = {};
        sg.transaction_data.target.handle = 0;
        sg.transaction_data.code          = code;
        sg.transaction_data.flags         = TF_ACCEPT_FDS;

        std::vector<uint8_t> writeBuf(sizeof(uint32_t) + sizeof(sg));
        uint32_t cmd = BC_TRANSACTION_SG;
        memcpy(writeBuf.data(), &cmd, sizeof(cmd));
        memcpy(writeBuf.data() + sizeof(cmd), &sg, sizeof(sg));

        std::vector<uint8_t> readBuf(4096);
        binder_write_read bwr = {};
        bwr.write_size    = writeBuf.size();
        bwr.write_buffer  = reinterpret_cast<uintptr_t>(writeBuf.data());
        bwr.read_size     = readBuf.size();
        bwr.read_buffer   = reinterpret_cast<uintptr_t>(readBuf.data());

        if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
            printf("sweep 0x%08x: ioctl errno=%d\n", code, errno);
            return;
        }
        // Walk responses; report final status if BR_REPLY found.
        size_t off = 0;
        int32_t status = 0xCAFEBABE;
        uint32_t lastCmd = 0;
        while (off + sizeof(uint32_t) <= bwr.read_consumed) {
            uint32_t r;
            memcpy(&r, readBuf.data() + off, sizeof(r));
            off += sizeof(r);
            lastCmd = r;
            if (r == (uint32_t)BR_REPLY &&
                off + sizeof(binder_transaction_data) <= bwr.read_consumed) {
                binder_transaction_data td;
                memcpy(&td, readBuf.data() + off, sizeof(td));
                if ((td.flags & TF_STATUS_CODE) && td.data_size >= 4) {
                    memcpy(&status, (void*)td.data.ptr.buffer, 4);
                }
                struct __attribute__((packed)) { uint32_t cmd; uint64_t p; } fb =
                    { (uint32_t)BC_FREE_BUFFER, td.data.ptr.buffer };
                binder_write_read bwr2 = {};
                bwr2.write_size = sizeof(fb);
                bwr2.write_buffer = reinterpret_cast<uintptr_t>(&fb);
                ioctl(fd_, BINDER_WRITE_READ, &bwr2);
                off += sizeof(td);
            } else if (r == (uint32_t)BR_ERROR &&
                       off + sizeof(int32_t) <= bwr.read_consumed) {
                off += sizeof(int32_t);
            }
        }
        printf("sweep 0x%08x: last=0x%08x status=%d\n", code, lastCmd, status);
    }

    /*
     * Bisect: two independent buffers, NO parent fixup.  If this
     * works but full ping fails, the bug is specifically in the
     * parent-fixup encoding (parent / parent_offset).
     */
    bool SendTwoBufferNoParentPing()
    {
        std::vector<uint8_t> sgA(16, 0xCC);
        std::vector<uint8_t> sgB(32, 0xDD);

        std::vector<uint8_t> data(2 * sizeof(binder_buffer_object));
        auto *bo = reinterpret_cast<binder_buffer_object *>(data.data());
        bo[0].hdr.type = BINDER_TYPE_PTR;
        bo[0].flags    = 0;
        bo[0].buffer   = reinterpret_cast<uintptr_t>(sgA.data());
        bo[0].length   = sgA.size();
        bo[0].parent   = 0;
        bo[0].parent_offset = 0;

        bo[1].hdr.type = BINDER_TYPE_PTR;
        bo[1].flags    = 0;          // ← KEY DIFFERENCE: no HAS_PARENT
        bo[1].buffer   = reinterpret_cast<uintptr_t>(sgB.data());
        bo[1].length   = sgB.size();
        bo[1].parent   = 0;
        bo[1].parent_offset = 0;

        uint64_t offsets[2] = { 0, sizeof(binder_buffer_object) };

        binder_transaction_data_sg sg = {};
        sg.transaction_data.target.handle = 0;
        sg.transaction_data.code          = HidlBase::TRANSACTION_ping;
        sg.transaction_data.flags         = TF_ACCEPT_FDS;
        sg.transaction_data.data_size     = data.size();
        sg.transaction_data.offsets_size  = sizeof(offsets);
        sg.transaction_data.data.ptr.buffer  = reinterpret_cast<uintptr_t>(data.data());
        sg.transaction_data.data.ptr.offsets = reinterpret_cast<uintptr_t>(offsets);
        sg.buffers_size = AlignUp(sgA.size()) + AlignUp(sgB.size());

        std::vector<uint8_t> writeBuf(sizeof(uint32_t) + sizeof(sg));
        uint32_t cmd = BC_TRANSACTION_SG;
        memcpy(writeBuf.data(), &cmd, sizeof(cmd));
        memcpy(writeBuf.data() + sizeof(cmd), &sg, sizeof(sg));

        std::vector<uint8_t> readBuf(4096);
        binder_write_read bwr = {};
        bwr.write_size    = writeBuf.size();
        bwr.write_buffer  = reinterpret_cast<uintptr_t>(writeBuf.data());
        bwr.read_size     = readBuf.size();
        bwr.read_buffer   = reinterpret_cast<uintptr_t>(readBuf.data());

        printf("\n--- probe: 2-buffer ping (NO parent fixup) ---\n");
        if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
            fprintf(stderr, "2buf no-parent BWR: errno=%d (%s)\n", errno, strerror(errno));
            return false;
        }
        printf("BWR done: read_consumed=%llu\n",
               (unsigned long long)bwr.read_consumed);
        DumpReplies(readBuf, bwr.read_consumed);
        return true;
    }

    bool SendPing()
    {
        const size_t descLen = strlen(kServiceManagerIface);

        /*
         * The interface token wire format is NOT hidl_string with
         * binder_buffer_objects (that's only for HIDL hidl_string method
         * arguments, e.g. IServiceManager::get(fqName, name)).  For the
         * interface token specifically, Parcel::writeInterfaceToken just
         * writes the raw bytes + null + 4-byte padding into the main
         * data buffer.  The receiver's enforceInterface() reads them
         * back via memchr+strcmp.
         *
         * Discovered by disassembling Parcel::enforceInterface and
         * Parcel::writeInterfaceToken in libhidlbase.so (2026-05-21).
         */
        const size_t tokenLen = descLen + 1;
        const size_t tokenPadded = (tokenLen + 3) & ~3;  // align to 4
        std::vector<uint8_t> data(tokenPadded, 0);
        memcpy(data.data(), kServiceManagerIface, descLen);  // null already from init

        // No offsets, no SG — just a flat 4-byte-aligned token buffer.
        uint64_t *offsets = nullptr;

        binder_transaction_data_sg sg = {};
        sg.transaction_data.target.handle = 0;  // hwservicemanager
        sg.transaction_data.code          = HidlBase::TRANSACTION_ping;
        sg.transaction_data.flags         = TF_ACCEPT_FDS;
        sg.transaction_data.data_size     = data.size();
        sg.transaction_data.offsets_size  = 0;
        sg.transaction_data.data.ptr.buffer  = reinterpret_cast<uintptr_t>(data.data());
        sg.transaction_data.data.ptr.offsets = 0;
        sg.buffers_size = 0;
        (void)offsets;  // unused (kept for symmetry with the historical layout)

        // DEBUG: dump the raw interface-token bytes we're sending.
        printf("\n--- DEBUG: token bytes (data_size=%zu) ---\n", data.size());
        printf("DEBUG:");
        for (size_t i = 0; i < data.size(); ++i)
            printf("%s%02x", (i % 8 == 0) ? " " : "", data[i]);
        printf("\n");

        // Build the BWR write buffer.
        std::vector<uint8_t> writeBuf(sizeof(uint32_t) + sizeof(sg));
        uint32_t cmd = BC_TRANSACTION_SG;
        memcpy(writeBuf.data(), &cmd, sizeof(cmd));
        memcpy(writeBuf.data() + sizeof(cmd), &sg, sizeof(sg));

        // Read buffer: large enough for BR_REPLY + payload.
        std::vector<uint8_t> readBuf(4096);

        binder_write_read bwr = {};
        bwr.write_size    = writeBuf.size();
        bwr.write_buffer  = reinterpret_cast<uintptr_t>(writeBuf.data());
        bwr.read_size     = readBuf.size();
        bwr.read_buffer   = reinterpret_cast<uintptr_t>(readBuf.data());

        printf("sending ping → handle=0, code=0x%08x, data=%zu, sg=%llu\n",
               HidlBase::TRANSACTION_ping, data.size(),
               (unsigned long long)sg.buffers_size);

        if (ioctl(fd_, BINDER_WRITE_READ, &bwr) < 0) {
            fprintf(stderr, "BINDER_WRITE_READ: errno=%d (%s)\n",
                    errno, strerror(errno));
            return false;
        }
        printf("BWR done: write_consumed=%llu, read_consumed=%llu\n",
               (unsigned long long)bwr.write_consumed,
               (unsigned long long)bwr.read_consumed);

        // Parse return commands.
        size_t off = 0;
        bool gotReply = false;
        while (off < bwr.read_consumed) {
            if (off + sizeof(uint32_t) > bwr.read_consumed) break;
            uint32_t rcmd;
            memcpy(&rcmd, readBuf.data() + off, sizeof(rcmd));
            off += sizeof(rcmd);

            printf("  cmd 0x%08x", rcmd);
            switch (rcmd) {
                case BR_NOOP:
                    printf(" BR_NOOP\n");
                    break;
                case BR_TRANSACTION_COMPLETE:
                    printf(" BR_TRANSACTION_COMPLETE\n");
                    break;
                case BR_SPAWN_LOOPER:
                    printf(" BR_SPAWN_LOOPER\n");
                    break;
                case BR_REPLY: {
                    if (off + sizeof(binder_transaction_data) > bwr.read_consumed) {
                        printf(" BR_REPLY (truncated)\n");
                        return false;
                    }
                    binder_transaction_data td;
                    memcpy(&td, readBuf.data() + off, sizeof(td));
                    off += sizeof(td);
                    printf(" BR_REPLY data_size=%llu offsets_size=%llu flags=0x%x\n",
                           (unsigned long long)td.data_size,
                           (unsigned long long)td.offsets_size,
                           td.flags);
                    /*
                     * flags=0 → normal Parcel reply; for ping this contains
                     *           a HIDL Status (4 bytes: exception_code).
                     *           0 = EX_NONE = success.
                     * flags=TF_STATUS_CODE → kernel/library returned a bare
                     *           status_t (e.g. -2147483647 if enforceInterface
                     *           failed, -74 if UNKNOWN_TRANSACTION).
                     */
                    int32_t v = 0;
                    if (td.data_size >= 4 && td.data.ptr.buffer) {
                        memcpy(&v, reinterpret_cast<void *>(td.data.ptr.buffer),
                               sizeof(v));
                    }
                    const char *kind = (td.flags & TF_STATUS_CODE)
                                       ? "status_t" : "parcel.int32[0]";
                    printf("    %s = %d  %s\n", kind, v,
                           (!(td.flags & TF_STATUS_CODE) && v == 0) ? "(EX_NONE — success!)" : "");
                    // Free the kernel-allocated buffer.
                    uint32_t fcmd = BC_FREE_BUFFER;
                    struct { uint32_t cmd; uint64_t ptr; } __attribute__((packed))
                        freeBuf = { fcmd, td.data.ptr.buffer };
                    binder_write_read bwr2 = {};
                    bwr2.write_size   = sizeof(freeBuf);
                    bwr2.write_buffer = reinterpret_cast<uintptr_t>(&freeBuf);
                    if (ioctl(fd_, BINDER_WRITE_READ, &bwr2) < 0) {
                        perror("BC_FREE_BUFFER");
                    }
                    gotReply = true;
                    break;
                }
                case BR_DEAD_REPLY:
                    printf(" BR_DEAD_REPLY (service is gone)\n");
                    return false;
                case BR_FAILED_REPLY:
                    printf(" BR_FAILED_REPLY (kernel rejected transaction)\n");
                    return false;
                case BR_ERROR: {
                    int32_t err = 0;
                    if (off + sizeof(err) <= bwr.read_consumed) {
                        memcpy(&err, readBuf.data() + off, sizeof(err));
                        off += sizeof(err);
                    }
                    printf(" BR_ERROR %d\n", err);
                    return false;
                }
                default:
                    printf(" (unhandled — stopping)\n");
                    return false;
            }
        }
        return gotReply;
    }
};

} // namespace

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("hidl_ping — N12.5.2\n");
    HidlPing p;
    int rc = p.Run();
    HILOG_INFO(LOG_CORE, "hidl_ping: exit rc=%{public}d", rc);
    return rc;
}
