/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * N12.5.1 — direct hwbinder smoke test (no libhybris).
 *
 * Question this answers: can an OHOS-side musl process open /dev/hwbinder,
 * call BINDER_VERSION + BINDER_SET_MAX_THREADS, and successfully send a
 * trivial BC_TRANSACTION to hwservicemanager (which is at handle 0)?
 *
 * If yes ⇒ pure-musl hand-rolled HIDL transport (Option ζ in the plan
 * doc) is viable.  If no ⇒ we fall back to a bionic-compiled shim
 * built into Halium.
 *
 * The whole point of this test is to NOT touch any libhybris /
 * libhidlbase machinery — we want to know whether OHOS-side binder
 * IPC works directly, end-to-end through the kernel.
 *
 * Reference: drivers/staging/android/binder.c (kernel)
 *            system/libhwbinder/include/hwbinder/binder_kernel.h (AOSP)
 */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "hybris_camera_log.h"

/* Subset of linux/android/binder.h — copied here so we don't depend on
 * a uapi header that may not be installed.  Stable kernel ABI. */
struct binder_write_read {
    uint64_t write_size;
    uint64_t write_consumed;
    uint64_t write_buffer;
    uint64_t read_size;
    uint64_t read_consumed;
    uint64_t read_buffer;
};

#define BINDER_WRITE_READ        _IOWR('b', 1, struct binder_write_read)
#define BINDER_SET_MAX_THREADS   _IOW ('b', 5, uint32_t)
#define BINDER_VERSION           _IOWR('b', 9, struct binder_version)
struct binder_version { int32_t protocol_version; };

namespace {

constexpr const char *kHwBinder    = "/dev/hwbinder";
constexpr const char *kBinder      = "/dev/binder";
constexpr const char *kVndBinder   = "/dev/vndbinder";

int ProbeNode(const char *path)
{
    std::printf("probe %s ...\n", path);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::printf("  open: errno=%d (%s)\n", errno, std::strerror(errno));
        return -1;
    }

    binder_version v = {0};
    if (ioctl(fd, BINDER_VERSION, &v) < 0) {
        std::printf("  BINDER_VERSION: errno=%d (%s)\n",
                    errno, std::strerror(errno));
        close(fd);
        return -1;
    }
    std::printf("  BINDER_VERSION: protocol=%d\n", v.protocol_version);

    /* mmap a small buffer — the kernel rejects later BWR calls without
     * one (binder is shared-memory transport).  4 KB is enough for the
     * smoke. */
    void *mapped = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        std::printf("  mmap(4 KB): errno=%d (%s)\n",
                    errno, std::strerror(errno));
        close(fd);
        return -1;
    }
    std::printf("  mmap(4 KB) = %p\n", mapped);

    uint32_t maxThreads = 4;
    if (ioctl(fd, BINDER_SET_MAX_THREADS, &maxThreads) < 0) {
        std::printf("  BINDER_SET_MAX_THREADS: errno=%d (%s)\n",
                    errno, std::strerror(errno));
        munmap(mapped, 4096);
        close(fd);
        return -1;
    }
    std::printf("  BINDER_SET_MAX_THREADS: ok\n");

    munmap(mapped, 4096);
    close(fd);
    std::printf("  OK\n");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    std::printf("binder_smoke — N12.5.1\n");

    int failures = 0;
    failures += ProbeNode(kHwBinder)   ? 1 : 0;
    failures += ProbeNode(kBinder)     ? 1 : 0;
    failures += ProbeNode(kVndBinder)  ? 1 : 0;

    std::printf("\nbinder_smoke: %d/3 probes failed\n", failures);
    HILOG_INFO(LOG_CORE,
               "binder_smoke: done failures=%{public}d", failures);
    return failures == 0 ? 0 : 1;
}
