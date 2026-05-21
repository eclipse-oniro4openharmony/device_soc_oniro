/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include "hw_camera_device_callback.h"

#include "hybris_camera_log.h"

namespace OHOS::Camera::Hybris::Hidl {

int32_t HwCameraDeviceCallback::OnTransact(uint32_t code,
                                           HwParcel::Reader &data,
                                           HwParcel *reply)
{
    /*
     * HIDL convention: every reply begins with an int32 exception
     * code (0 = EX_NONE).  For status-only methods that's the whole
     * reply.  For value-returning methods (interfaceDescriptor,
     * interfaceChain, getDebugInfo, …) the exception code is
     * followed by the marshalled return value.
     *
     * For now we accept everything Halium sends us and return
     * EX_NONE empty reply — the actual frame-delivery dispatch
     * (processCaptureResult) is wired in N12.7.
     */
    (void)data;

    /* Camera-specific method codes (per BpHwCameraDeviceCallback). */
    if (code == TRANSACTION_processCaptureResult ||
        code == TRANSACTION_processCaptureResult_2_6) {
        CAMERA_VDI_LOGI("ICameraDeviceCallback::processCaptureResult "
                        "(code=%{public}u, %{public}zuB) — N12.7 stub",
                        code, data.DataSize());
        reply->WriteInt32(0);   // EX_NONE
        return 0;
    }
    if (code == TRANSACTION_notify) {
        CAMERA_VDI_LOGI("ICameraDeviceCallback::notify (%{public}zuB) — "
                        "N12.7 stub", data.DataSize());
        reply->WriteInt32(0);
        return 0;
    }
    if (code == TRANSACTION_requestStreamBuffers) {
        CAMERA_VDI_LOGW("ICameraDeviceCallback::requestStreamBuffers "
                        "— not implemented yet; returning error");
        /*
         * V3.5+ HAL buffer manager flow.  Reply with
         * BufferRequestStatus::FAILED_UNKNOWN (0xFFFFFFFF) so HAL falls
         * back to client-supplied buffers.  TODO: write the proper
         * (Status, vec<StreamBufferRet>, BufferRequestStatus) reply.
         */
        reply->WriteInt32(0);
        return 0;
    }
    if (code == TRANSACTION_returnStreamBuffers) {
        CAMERA_VDI_LOGW("ICameraDeviceCallback::returnStreamBuffers — "
                        "not implemented yet");
        reply->WriteInt32(0);
        return 0;
    }

    /* Otherwise it's an inherited IBase method. */
    return HandleIBaseMethod(code, data, reply);
}

int32_t HwCameraDeviceCallback::HandleIBaseMethod(uint32_t code,
                                                   HwParcel::Reader &data,
                                                   HwParcel *reply)
{
    (void)data;
    using namespace HidlBase;

    switch (code) {
        case TRANSACTION_ping:
            /* `ping()` generates () — empty reply with just EX_NONE. */
            reply->WriteInt32(0);
            return 0;
        case TRANSACTION_linkToDeath:
            /* `linkToDeath(recipient, cookie)` generates (bool success).
             * Halium calls this right after `open()`.  We don't
             * actually monitor death; reply with success=true.
             */
            reply->WriteInt32(0);
            reply->WriteBool(true);
            return 0;
        case TRANSACTION_unlinkToDeath:
            reply->WriteInt32(0);
            reply->WriteBool(true);
            return 0;
        case TRANSACTION_notifySyspropsChanged:
            reply->WriteInt32(0);
            return 0;
        case TRANSACTION_interfaceDescriptor:
            /*
             * Generates (hidl_string).  Reply with our most-derived
             * descriptor.  Halium might call this to verify type
             * compatibility right after open().
             */
            reply->WriteInt32(0);
            reply->WriteHidlString(kDescriptorV3_4);
            return 0;
        case TRANSACTION_interfaceChain:
        case TRANSACTION_getHashChain:
        case TRANSACTION_getDebugInfo:
        case TRANSACTION_debug:
        case TRANSACTION_setHALInstrumentation:
            /*
             * Unlikely-to-be-called IBase methods.  We don't have
             * hidl_vec<hidl_string> / hidl_array<uint8,32> / DebugInfo
             * writers wired yet.  Return EX_NONE and hope Halium
             * doesn't actually try to parse the return value (which
             * it won't if it's just probing).
             *
             * If any of these turn out to be required, add a proper
             * writer in HwParcel and a real reply here.
             */
            CAMERA_VDI_LOGW("ICameraDeviceCallback: IBase code "
                            "0x%{public}08x — stub empty reply", code);
            reply->WriteInt32(0);
            return 0;
        default:
            CAMERA_VDI_LOGE("ICameraDeviceCallback: unknown TX code "
                            "0x%{public}08x", code);
            return -EINVAL;
    }
}

} // namespace OHOS::Camera::Hybris::Hidl
