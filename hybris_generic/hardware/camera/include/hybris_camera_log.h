/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#ifndef HYBRIS_CAMERA_LOG_H
#define HYBRIS_CAMERA_LOG_H

#include "hilog/log.h"

#undef LOG_TAG
#define LOG_TAG "CAMERA_VDI"
#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD002510

#define CAMERA_VDI_LOGD(fmt, ...) HILOG_DEBUG(LOG_CORE, fmt, ##__VA_ARGS__)
#define CAMERA_VDI_LOGI(fmt, ...) HILOG_INFO(LOG_CORE,  fmt, ##__VA_ARGS__)
#define CAMERA_VDI_LOGW(fmt, ...) HILOG_WARN(LOG_CORE,  fmt, ##__VA_ARGS__)
#define CAMERA_VDI_LOGE(fmt, ...) HILOG_ERROR(LOG_CORE, fmt, ##__VA_ARGS__)

#endif
