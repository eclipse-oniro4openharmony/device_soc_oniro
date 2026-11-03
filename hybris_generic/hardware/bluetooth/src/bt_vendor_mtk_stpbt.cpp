/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Bluetooth vendor library for hybris_generic: the MediaTek connsys
 * controller behind /dev/stpbt.
 *
 * The OHOS HCI HDI (drivers/peripheral/bluetooth/hci) dlopens
 * libbt_vendor.z.so and asks it for HCI channel fds.  On MediaTek's connac
 * parts (MT6878 on the Volla Phone Plinius: bt_drv_6878 over conninfra) the
 * kernel driver does all of the bring-up that a UART vendor library usually
 * does: open() powers connsys on, downloads the patch, applies BT_FW.cfg
 * and the antenna/power WMT commands, and close() powers it off again.
 * What is left is a plain byte stream carrying H4 framing, which the HDI's
 * own H4Protocol already speaks, so this library only has to hand over one
 * fd.
 *
 * The one thing the driver does not do is the device address: the
 * controller comes up with MediaTek's placeholder (00:00:46:xx:xx:xx).  The
 * Android stack programs the factory address out of NVRAM with the vendor
 * command 0xFC1A; we do the same with the address the HDI passes to init()
 * (read from /data/vendor/bluetooth/btmac.txt, which androidd fills in from
 * the NVRAM BT_Addr record).  The address survives the host's HCI_Reset.
 *
 * Verified on the Plinius with a raw probe: split type/payload writes and
 * byte-wise reads both work on /dev/stpbt, which is how H4Protocol uses it.
 *
 * initCb must be asynchronous.  The stack (bluetooth_service, hci.c
 * HciInitHal) calls the HDI's Init and only after it returns arms a 5 s
 * alarm that SIGTERMs the whole service unless OnInited cancels it.
 * Reporting completion from inside BT_OP_INIT delivers OnInited before the
 * alarm exists, so nothing cancels it and the service kills itself 5 s
 * after every enable.  A real UART vendor library reports from its
 * firmware-download thread; we report from a short-lived one.
 */

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <hilog/log.h>

#include "ohos_bt_vendor_lib.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD000105
#define LOG_TAG "bt_vendor_mtk"

#define LOGI(fmt, ...) HILOG_INFO(LOG_CORE, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) HILOG_WARN(LOG_CORE, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, fmt, ##__VA_ARGS__)

namespace {
constexpr const char *STPBT_DEV = "/dev/stpbt";
constexpr int BD_ADDR_LEN = 6;
constexpr uint32_t LPM_IDLE_TIMEOUT_MS = 3000;

/* open() is where the driver powers connsys and loads firmware (~100 ms);
 * it returns EBUSY while a previous close() is still powering down. */
constexpr int OPEN_RETRIES = 20;
constexpr int OPEN_RETRY_US = 100 * 1000;
constexpr int CMD_TIMEOUT_MS = 2000;
/* Long enough for the stack's Init IPC to return and arm its alarm. */
constexpr auto INIT_REPORT_DELAY = std::chrono::milliseconds(200);

constexpr uint8_t H4_CMD = 0x01;
constexpr uint8_t H4_EVT = 0x04;
constexpr uint8_t EVT_CMD_COMPLETE = 0x0E;
constexpr uint8_t EVT_CMD_STATUS = 0x0F;
constexpr uint16_t MTK_OP_SET_BDADDR = 0xFC1A;

const BtVendorCallbacksT *g_callbacks = nullptr;
uint8_t g_bdaddr[BD_ADDR_LEN];
bool g_haveBdaddr = false;
int g_fd = -1;

std::mutex g_initLock;
std::condition_variable g_initCv;
bool g_initCancel = false;
std::thread g_initThread;

bool AddressUsable(const uint8_t *a)
{
    bool allZero = true;
    bool allOnes = true;
    for (int i = 0; i < BD_ADDR_LEN; i++) {
        allZero = allZero && a[i] == 0x00;
        allOnes = allOnes && a[i] == 0xFF;
    }
    /* The HDI's last-resort default when it cannot read or create
     * btmac.txt; programming it would give every unit the same address. */
    static const uint8_t hdiDefault[BD_ADDR_LEN] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    /* Bit 0 of the first byte marks a group address; the HDI's random
     * fallback (urandom bytes) can produce one, and it is no device's. */
    bool group = (a[0] & 0x01) != 0;
    return !allZero && !allOnes && !group && memcmp(a, hdiDefault, BD_ADDR_LEN) != 0;
}

int64_t NowMs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool WriteAll(int fd, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    int64_t deadline = NowMs() + CMD_TIMEOUT_MS;
    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            return false;
        }
        if (NowMs() > deadline) {
            return false;
        }
        struct pollfd p = {fd, POLLOUT, 0};
        (void)poll(&p, 1, 50);
    }
    return true;
}

/* Read exactly len bytes from the non-blocking fd before the deadline. */
bool ReadExact(int fd, uint8_t *buf, size_t len, int64_t deadline)
{
    size_t done = 0;
    while (done < len) {
        int64_t left = deadline - NowMs();
        if (left <= 0) {
            return false;
        }
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, static_cast<int>(left));
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        ssize_t n = read(fd, buf + done, len - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            return false;
        }
    }
    return true;
}

/*
 * Send one HCI command and wait for its Command Complete / Command Status.
 * Runs before the HDI starts watching the fd, so nothing else is reading.
 * Returns the HCI status byte, or -1 on transport failure / timeout.
 */
int SendCommandSync(int fd, uint16_t opcode, const uint8_t *params, uint8_t plen)
{
    uint8_t pkt[4 + 255];
    pkt[0] = H4_CMD;
    pkt[1] = static_cast<uint8_t>(opcode & 0xFF);
    pkt[2] = static_cast<uint8_t>(opcode >> 8);
    pkt[3] = plen;
    if (plen > 0) {
        memcpy(pkt + 4, params, plen);
    }
    if (!WriteAll(fd, pkt, 4 + plen)) {
        LOGE("cmd 0x%{public}04x: write failed: %{public}s", opcode, strerror(errno));
        return -1;
    }

    int64_t deadline = NowMs() + CMD_TIMEOUT_MS;
    for (;;) {
        uint8_t hdr[3];
        if (!ReadExact(fd, hdr, 1, deadline)) {
            LOGE("cmd 0x%{public}04x: no reply", opcode);
            return -1;
        }
        if (hdr[0] != H4_EVT) {
            /* Not expected before the stack is up; we cannot resync a
             * stream we do not understand. */
            LOGE("cmd 0x%{public}04x: unexpected H4 type 0x%{public}02x", opcode, hdr[0]);
            return -1;
        }
        if (!ReadExact(fd, hdr + 1, 2, deadline)) {
            return -1;
        }
        uint8_t body[255];
        if (!ReadExact(fd, body, hdr[2], deadline)) {
            return -1;
        }
        /* Command Complete: ncmd, opcode(2), status...
         * Command Status:   status, ncmd, opcode(2) */
        if (hdr[1] == EVT_CMD_COMPLETE && hdr[2] >= 4 &&
            (body[1] | (body[2] << 8)) == opcode) {
            return body[3];
        }
        if (hdr[1] == EVT_CMD_STATUS && hdr[2] >= 4 &&
            (body[2] | (body[3] << 8)) == opcode) {
            return body[0];
        }
        LOGW("cmd 0x%{public}04x: skipping event 0x%{public}02x", opcode, hdr[1]);
    }
}

void ProgramAddress(int fd)
{
    if (!g_haveBdaddr) {
        LOGW("no usable device address, controller keeps its default");
        return;
    }
    /* The HDI hands the address over in display order; HCI wants it
     * little-endian. */
    uint8_t le[BD_ADDR_LEN];
    for (int i = 0; i < BD_ADDR_LEN; i++) {
        le[i] = g_bdaddr[BD_ADDR_LEN - 1 - i];
    }
    int status = SendCommandSync(fd, MTK_OP_SET_BDADDR, le, BD_ADDR_LEN);
    if (status != 0) {
        LOGE("set device address failed (status %{public}d)", status);
        return;
    }
    LOGI("device address set to %{public}02X:%{public}02X:%{public}02X:**:**:%{public}02X",
         g_bdaddr[0], g_bdaddr[1], g_bdaddr[2], g_bdaddr[5]);
}

int OpenChannel(int *channels)
{
    if (channels == nullptr) {
        return -1;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }

    int fd = -1;
    for (int i = 0; i < OPEN_RETRIES; i++) {
        fd = open(STPBT_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0 || (errno != EBUSY && errno != EAGAIN)) {
            break;
        }
        usleep(OPEN_RETRY_US);
    }
    if (fd < 0) {
        LOGE("open %{public}s: %{public}s", STPBT_DEV, strerror(errno));
        return -1;
    }
    LOGI("%{public}s open, fd %{public}d", STPBT_DEV, fd);

    ProgramAddress(fd);

    g_fd = fd;
    for (int i = 0; i < HCI_MAX_CHANNEL; i++) {
        channels[i] = fd;
    }
    return 1; /* one fd: the HDI runs H4 on it */
}

void StopInitReport()
{
    {
        std::lock_guard<std::mutex> lock(g_initLock);
        g_initCancel = true;
    }
    g_initCv.notify_all();
    if (g_initThread.joinable()) {
        if (g_initThread.get_id() == std::this_thread::get_id()) {
            g_initThread.detach(); /* torn down from inside initCb */
        } else {
            g_initThread.join();
        }
    }
}

void StartInitReport()
{
    StopInitReport();
    g_initCancel = false;
    bool ok = g_fd >= 0;
    g_initThread = std::thread([ok]() {
        std::unique_lock<std::mutex> lock(g_initLock);
        if (g_initCv.wait_for(lock, INIT_REPORT_DELAY, [] { return g_initCancel; })) {
            return;
        }
        const BtVendorCallbacksT *cb = g_callbacks;
        lock.unlock();
        if (cb != nullptr && cb->initCb != nullptr) {
            LOGI("init done (%{public}s)", ok ? "success" : "fail");
            cb->initCb(ok ? BTC_OP_RESULT_SUCCESS : BTC_OP_RESULT_FAIL);
        }
    });
}

void CloseChannel()
{
    StopInitReport();
    if (g_fd >= 0) {
        close(g_fd); /* the driver powers connsys BT off here */
        g_fd = -1;
        LOGI("%{public}s closed", STPBT_DEV);
    }
}

int VendorInit(const BtVendorCallbacksT *cb, unsigned char *localBdaddr)
{
    if (cb == nullptr) {
        LOGE("init: no callbacks");
        return -1;
    }
    g_callbacks = cb;
    g_haveBdaddr = false;
    if (localBdaddr != nullptr && AddressUsable(localBdaddr)) {
        memcpy(g_bdaddr, localBdaddr, BD_ADDR_LEN);
        g_haveBdaddr = true;
    }
    LOGI("init (address %{public}s)", g_haveBdaddr ? "provided" : "unusable");
    return 0;
}

int VendorOp(BtOpcodeT opcode, void *param)
{
    switch (opcode) {
        case BT_OP_POWER_ON:
        case BT_OP_POWER_OFF:
            /* Power follows open()/close() of /dev/stpbt. */
            return 0;
        case BT_OP_HCI_CHANNEL_OPEN:
            return OpenChannel(static_cast<int *>(param));
        case BT_OP_HCI_CHANNEL_CLOSE:
            CloseChannel();
            return 0;
        case BT_OP_INIT:
            /* Firmware is already up and the address programmed; report
             * it asynchronously (see the header comment). */
            StartInitReport();
            return 0;
        case BT_OP_GET_LPM_TIMER:
            if (param != nullptr) {
                *static_cast<uint32_t *>(param) = LPM_IDLE_TIMEOUT_MS;
            }
            return 0;
        case BT_OP_LPM_ENABLE:
        case BT_OP_LPM_DISABLE:
        case BT_OP_WAKEUP_LOCK:
        case BT_OP_WAKEUP_UNLOCK:
            /* connsys handles its own sleep; nothing to toggle from here. */
            return 0;
        case BT_OP_EVENT_CALLBACK:
            return 0;
        default:
            return -1;
    }
}

void VendorClose()
{
    CloseChannel();
    g_callbacks = nullptr;
}
} // namespace

extern "C" __attribute__((visibility("default"))) const BtVendorInterfaceT BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(BtVendorInterfaceT),
    VendorInit,
    VendorOp,
    VendorClose,
};
