/*
 * Copyright (c) 2026 Oniro Authors
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
 */

/*
 * Diagnostic for the recents-thumbnail ASTC path: allocates buffers through
 * the display buffer VDI the way ImageSource's DMA memory does (BLOB and
 * RGBA), then reports what each fd in the resulting BufferHandle actually is
 * and whether it can be mmap'd, so the fd-mmap fallback used in mapper-less
 * (app sandbox) processes can be made to work.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buffer_handle.h"
#include "idisplay_buffer_vdi.h"

using namespace OHOS::HDI::Display::Buffer::V1_0;

static void InspectFd(const char* label, int fd)
{
    char linkPath[64];
    char target[128] = "?";
    (void)snprintf(linkPath, sizeof(linkPath), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(linkPath, target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
    }
    struct stat st = {};
    (void)fstat(fd, &st);
    printf("  %s fd=%d -> %s mode=%o size=%lld\n", label, fd, target,
           st.st_mode, static_cast<long long>(st.st_size));

    void* rw = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    printf("    mmap RW: %s\n", rw == MAP_FAILED ? strerror(errno) : "OK");
    if (rw != MAP_FAILED) {
        munmap(rw, 4096);
    }
    void* ro = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0);
    printf("    mmap RO: %s\n", ro == MAP_FAILED ? strerror(errno) : "OK");
    if (ro != MAP_FAILED) {
        munmap(ro, 4096);
    }
}

static void TestAlloc(IDisplayBufferVdi* vdi, const char* name, uint32_t w, uint32_t h,
                      uint32_t fmt, uint64_t usage)
{
    AllocInfo info = {};
    info.width = w;
    info.height = h;
    info.format = fmt;
    info.usage = usage;

    BufferHandle* bh = nullptr;
    int32_t ret = vdi->AllocMem(info, bh);
    printf("%s: AllocMem(%ux%u fmt=%u usage=0x%llx) ret=%d\n", name, w, h, fmt,
           static_cast<unsigned long long>(usage), ret);
    if (ret != 0 || bh == nullptr) {
        return;
    }
    printf("  handle: fd=%d stride=%d size=%d reserveFds=%u reserveInts=%u\n",
           bh->fd, bh->stride, bh->size, bh->reserveFds, bh->reserveInts);
    InspectFd("primary", bh->fd);
    for (uint32_t i = 0; i < bh->reserveFds; i++) {
        InspectFd("reserve", bh->reserve[i]);
    }

    /* Exercise the VDI's own Mmap/Unmap (fd fallback when
     * HYBRIS_DISP_FORCE_FD_MMAP is set in the environment). */
    void* addr = vdi->Mmap(*bh);
    if (addr == nullptr) {
        printf("  vdi Mmap: FAILED\n");
    } else {
        volatile uint8_t* p = static_cast<volatile uint8_t*>(addr);
        p[0] = 0xA5;
        p[bh->size - 1] = 0x5A;
        printf("  vdi Mmap: OK addr=%p write/read %s\n", addr,
               (p[0] == 0xA5 && p[bh->size - 1] == 0x5A) ? "OK" : "MISMATCH");
        vdi->Unmap(*bh);
    }
    vdi->FreeMem(*bh);
}

int main()
{
    IDisplayBufferVdi* vdi = CreateDisplayBufferVdi();
    if (vdi == nullptr) {
        printf("CreateDisplayBufferVdi failed\n");
        return 1;
    }

    /* ImageSource ASTC blob: CPU_READ|CPU_WRITE|MEM_DMA (bits 0,1,3) */
    TestAlloc(vdi, "BLOB astc-like", 648016, 1, 38, 0xBULL);
    /* Same with the HDR/no-padding extras ImageSource may add */
    TestAlloc(vdi, "BLOB no-ipc", 648016, 1, 38, 0xBULL | (1ULL << 45) | (1ULL << 46));
    /* Snapshot-style RGBA for comparison */
    TestAlloc(vdi, "RGBA_8888", 1080, 2400, 12, 0xBULL | (1ULL << 8) | (1ULL << 9));

    DestroyDisplayBufferVdi(vdi);
    return 0;
}
