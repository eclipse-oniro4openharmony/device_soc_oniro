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
 */

/*
 * hybris_refresh_test: drive the screen's refresh rate through RenderService.
 *
 *   hybris_refresh_test                 show modes, current rate and rate mode
 *   hybris_refresh_test mode <id>       SetScreenActiveMode(screen 0, id)
 *   hybris_refresh_test rate <hz>       SetScreenRefreshRate(screen 0, scene 0, hz)
 *   hybris_refresh_test ratemode <m>    SetRefreshRateMode(m)  (-1 = dynamic)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "transaction/rs_interfaces.h"

using namespace OHOS::Rosen;

int main(int argc, char **argv)
{
    auto &rs = RSInterfaces::GetInstance();
    ScreenId id = rs.GetDefaultScreenId();
    if (argc >= 3) {
        int value = atoi(argv[2]);
        if (strcmp(argv[1], "mode") == 0) {
            printf("SetScreenActiveMode(%llu, %d) -> %u\n", static_cast<unsigned long long>(id), value,
                rs.SetScreenActiveMode(id, static_cast<uint32_t>(value)));
        } else if (strcmp(argv[1], "rate") == 0) {
            rs.SetScreenRefreshRate(id, 0, value);
            printf("SetScreenRefreshRate(%llu, 0, %d)\n", static_cast<unsigned long long>(id), value);
        } else if (strcmp(argv[1], "ratemode") == 0) {
            rs.SetRefreshRateMode(value);
            printf("SetRefreshRateMode(%d)\n", value);
        } else {
            fprintf(stderr, "unknown command %s\n", argv[1]);
            return 2;
        }
        usleep(500000);
    }
    for (const auto &m : rs.GetScreenSupportedModes(id)) {
        printf("mode %d: %dx%d @ %u Hz\n", m.GetScreenModeId(), m.GetScreenWidth(), m.GetScreenHeight(),
            m.GetScreenRefreshRate());
    }
    auto active = rs.GetScreenActiveMode(id);
    printf("active mode %d (%u Hz); current refresh rate %u Hz; refresh rate mode %d\n",
        active.GetScreenModeId(), active.GetScreenRefreshRate(), rs.GetScreenCurrentRefreshRate(id),
        rs.GetCurrentRefreshRateMode());
    return 0;
}
