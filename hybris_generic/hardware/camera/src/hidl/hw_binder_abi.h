/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * Vendored Linux binder uapi.  Subset of include/uapi/linux/android/binder.h
 * sufficient for the HIDL transport in libhybris_hidl.  Kept here so the
 * transport library has no dependency on OHOS's ipc subsystem or on any
 * kernel-headers package being installed at build time.
 */

#ifndef HYBRIS_HIDL_HW_BINDER_ABI_H
#define HYBRIS_HIDL_HW_BINDER_ABI_H

#include <cstdint>
#include <sys/ioctl.h>

namespace OHOS::Camera::Hybris::Hidl {

using binder_size_t    = uint64_t;
using binder_uintptr_t = uint64_t;

#define B_TYPE_LARGE   0x85
#define B_PACK_CHARS(c1, c2, c3, c4) \
    ((((c1) << 24)) | (((c2) << 16)) | (((c3) << 8)) | (c4))

enum {
    BINDER_TYPE_BINDER       = B_PACK_CHARS('s', 'b', '*', B_TYPE_LARGE),
    BINDER_TYPE_WEAK_BINDER  = B_PACK_CHARS('w', 'b', '*', B_TYPE_LARGE),
    BINDER_TYPE_HANDLE       = B_PACK_CHARS('s', 'h', '*', B_TYPE_LARGE),
    BINDER_TYPE_WEAK_HANDLE  = B_PACK_CHARS('w', 'h', '*', B_TYPE_LARGE),
    BINDER_TYPE_FD           = B_PACK_CHARS('f', 'd', '*', B_TYPE_LARGE),
    BINDER_TYPE_FDA          = B_PACK_CHARS('f', 'd', 'a', B_TYPE_LARGE),
    BINDER_TYPE_PTR          = B_PACK_CHARS('p', 't', '*', B_TYPE_LARGE),
};

struct binder_object_header {
    uint32_t type;
};

struct flat_binder_object {
    binder_object_header hdr;
    uint32_t flags;
    union {
        binder_uintptr_t binder;
        uint32_t handle;
    };
    binder_uintptr_t cookie;
};

struct binder_fd_object {
    binder_object_header hdr;
    uint32_t pad_flags;
    union {
        binder_uintptr_t pad_binder;
        uint32_t fd;
    };
    binder_uintptr_t cookie;
};

struct binder_buffer_object {
    binder_object_header hdr;
    uint32_t flags;
    binder_uintptr_t buffer;
    binder_size_t length;
    binder_size_t parent;
    binder_size_t parent_offset;
};

enum {
    BINDER_BUFFER_FLAG_HAS_PARENT = 0x01,
};

struct binder_write_read {
    binder_size_t    write_size;
    binder_size_t    write_consumed;
    binder_uintptr_t write_buffer;
    binder_size_t    read_size;
    binder_size_t    read_consumed;
    binder_uintptr_t read_buffer;
};

struct binder_version {
    int32_t protocol_version;
};

#define BINDER_WRITE_READ      _IOWR('b', 1, binder_write_read)
#define BINDER_SET_MAX_THREADS _IOW ('b', 5, uint32_t)
#define BINDER_VERSION         _IOWR('b', 9, binder_version)

enum transaction_flags {
    TF_ONE_WAY     = 0x01,
    TF_ROOT_OBJECT = 0x04,
    TF_STATUS_CODE = 0x08,
    TF_ACCEPT_FDS  = 0x10,
};

struct binder_transaction_data {
    union {
        uint32_t handle;
        binder_uintptr_t ptr;
    } target;
    binder_uintptr_t cookie;
    uint32_t code;
    uint32_t flags;
    int32_t  sender_pid;
    uint32_t sender_euid;
    binder_size_t data_size;
    binder_size_t offsets_size;
    union {
        struct {
            binder_uintptr_t buffer;
            binder_uintptr_t offsets;
        } ptr;
        uint8_t buf[8];
    } data;
};

struct binder_transaction_data_sg {
    binder_transaction_data transaction_data;
    binder_size_t           buffers_size;
};

struct binder_ptr_cookie {
    binder_uintptr_t ptr;
    binder_uintptr_t cookie;
};

enum binder_driver_return_protocol {
    BR_ERROR               = _IOR('r', 0, int32_t),
    BR_OK                  = _IO ('r', 1),
    BR_TRANSACTION         = _IOR('r', 2, binder_transaction_data),
    BR_REPLY               = _IOR('r', 3, binder_transaction_data),
    BR_DEAD_REPLY          = _IO ('r', 5),
    BR_TRANSACTION_COMPLETE= _IO ('r', 6),
    BR_INCREFS             = _IOR('r', 7,  binder_ptr_cookie),
    BR_ACQUIRE             = _IOR('r', 8,  binder_ptr_cookie),
    BR_RELEASE             = _IOR('r', 9,  binder_ptr_cookie),
    BR_DECREFS             = _IOR('r', 10, binder_ptr_cookie),
    BR_NOOP                = _IO ('r', 12),
    BR_SPAWN_LOOPER        = _IO ('r', 13),
    BR_DEAD_BINDER         = _IOR('r', 15, binder_uintptr_t),
    BR_FAILED_REPLY        = _IO ('r', 17),
};

enum binder_driver_command_protocol {
    BC_TRANSACTION       = _IOW('c', 0,  binder_transaction_data),
    BC_REPLY             = _IOW('c', 1,  binder_transaction_data),
    BC_FREE_BUFFER       = _IOW('c', 3,  binder_uintptr_t),
    BC_INCREFS           = _IOW('c', 4,  uint32_t),
    BC_ACQUIRE           = _IOW('c', 5,  uint32_t),
    BC_RELEASE           = _IOW('c', 6,  uint32_t),
    BC_DECREFS           = _IOW('c', 7,  uint32_t),
    BC_INCREFS_DONE      = _IOW('c', 8,  binder_ptr_cookie),
    BC_ACQUIRE_DONE      = _IOW('c', 9,  binder_ptr_cookie),
    BC_REGISTER_LOOPER   = _IO ('c', 11),
    BC_ENTER_LOOPER      = _IO ('c', 12),
    BC_EXIT_LOOPER       = _IO ('c', 13),
    BC_TRANSACTION_SG    = _IOW('c', 17, binder_transaction_data_sg),
    BC_REPLY_SG          = _IOW('c', 18, binder_transaction_data_sg),
};

/*
 * HIDL IBase transaction codes.  NOT 0xff000000+N as some sources claim;
 * the libhidlbase ABI uses the magic top byte 0x0F followed by a 3-byte
 * ASCII mnemonic of the method.  Discovered by disassembling
 * BnHwBase::onTransact in libhidlbase.so on the X23 (2026-05-21).
 *
 * Encoding: (0x0F << 24) | (c0 << 16) | (c1 << 8) | c2
 *
 * Confirmed codes from the dispatch tree:
 */
namespace HidlBase {
    constexpr uint32_t TRANSACTION_interfaceChain        = 0x0F43484E; // CHN
    constexpr uint32_t TRANSACTION_debug                 = 0x0F444247; // DBG
    constexpr uint32_t TRANSACTION_interfaceDescriptor   = 0x0F445343; // DSC
    constexpr uint32_t TRANSACTION_getHashChain          = 0x0F485348; // HSH
    constexpr uint32_t TRANSACTION_setHALInstrumentation = 0x0F494E54; // INT (= 'instrument')
    constexpr uint32_t TRANSACTION_linkToDeath           = 0x0F4C5444; // LTD
    constexpr uint32_t TRANSACTION_ping                  = 0x0F504E47; // PNG
    constexpr uint32_t TRANSACTION_getDebugInfo          = 0x0F524546; // REF (= 'reflect'?)
    constexpr uint32_t TRANSACTION_notifySyspropsChanged = 0x0F535953; // SYS
    constexpr uint32_t TRANSACTION_unlinkToDeath         = 0x0F555444; // UTD
}

} // namespace OHOS::Camera::Hybris::Hidl

#endif // HYBRIS_HIDL_HW_BINDER_ABI_H
