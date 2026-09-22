/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See wdbinder.h.  Wire format notes are next to the code that depends on
 * them; the kernel side is drivers/android/binder.c.
 */

#include "wdbinder.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/android/binder.h>

/* Newer than the uapi headers some sysroots ship; the numbers are ABI. */
#ifndef BR_FROZEN_REPLY
#define BR_FROZEN_REPLY _IO('r', 18)
#endif
#ifndef BR_TRANSACTION_SEC_CTX
#define BR_TRANSACTION_SEC_CTX _IOR('r', 2, struct binder_transaction_data_secctx)
#endif

#define WDB_MAP_SIZE (1024 * 1024)
#define WDB_MAX_REPLY (4 * 1024 * 1024)
#define WDB_MAX_LIST 4096

#define IFACE_SERVICE_MANAGER "android.os.IServiceManager"
#define IFACE_PLATFORM "lineageos.waydroid.IPlatform"
#define SERVICE_PLATFORM "waydroidplatform"

/*
 * IServiceManager transaction 2.  Up to Android 14 that is checkService
 * (returns a nullable IBinder); from Android 15 the AIDL gained getService2
 * in that slot, which returns a `Service` union wrapping the same binder.
 * We never parse the reply body: the binder object is located through the
 * kernel's offsets array, which is identical for both, so one code path
 * serves the Android 13 and the Android 16 image.
 */
#define SM_LOOKUP 2

enum {
    PLATFORM_GETPROP = 1,
    PLATFORM_SETPROP = 2,
    PLATFORM_GET_APPS_INFO = 3,
    PLATFORM_GET_APP_INFO = 4,
    PLATFORM_INSTALL_APP = 5,
    PLATFORM_REMOVE_APP = 6,
    PLATFORM_LAUNCH_APP = 7,
    PLATFORM_GET_APP_NAME = 8,
    PLATFORM_SETTINGS_PUT_STRING = 9,
    PLATFORM_SETTINGS_GET_STRING = 10,
    PLATFORM_SETTINGS_PUT_INT = 11,
    PLATFORM_SETTINGS_GET_INT = 12,
    PLATFORM_LAUNCH_INTENT = 13,
};

struct wdb {
    char path[256];
    int fd;
    void *map;
    int have_platform;
    uint32_t platform;
};

const char *wdb_strerror(int err)
{
    switch (err) {
        case WDB_OK: return "ok";
        case WDB_EOPEN: return "cannot open the container's binder (not running?)";
        case WDB_ENOSERVICE: return "service not registered (Android still booting?)";
        case WDB_ETIMEDOUT: return "timed out (container frozen or wedged?)";
        case WDB_EDEAD: return "remote died";
        case WDB_EPROTO: return "protocol error";
        case WDB_EREMOTE: return "remote exception";
        case WDB_ENOMEM: return "out of memory";
        case WDB_EINVAL: return "invalid argument";
        default: return "unknown error";
    }
}

/* ---- parcel -------------------------------------------------------------- */

void wdb_parcel_init(struct wdb_parcel *p)
{
    memset(p, 0, sizeof *p);
}

void wdb_parcel_free(struct wdb_parcel *p)
{
    free(p->data);
    memset(p, 0, sizeof *p);
}

static uint8_t *parcel_grow(struct wdb_parcel *p, size_t n)
{
    if (p->error) {
        return NULL;
    }
    if (n > WDB_MAX_REPLY || p->len + n > WDB_MAX_REPLY) {
        p->error = WDB_EINVAL;
        return NULL;
    }
    if (p->len + n > p->cap) {
        size_t cap = p->cap ? p->cap : 256;
        while (cap < p->len + n) {
            cap *= 2;
        }
        uint8_t *d = realloc(p->data, cap);
        if (d == NULL) {
            p->error = WDB_ENOMEM;
            return NULL;
        }
        p->data = d;
        p->cap = cap;
    }
    uint8_t *at = p->data + p->len;
    memset(at, 0, n);
    p->len += n;
    return at;
}

void wdb_put_i32(struct wdb_parcel *p, int32_t v)
{
    uint8_t *at = parcel_grow(p, 4);
    if (at != NULL) {
        memcpy(at, &v, 4);
    }
}

/* One code point from UTF-8; malformed input yields U+FFFD and advances. */
static uint32_t utf8_next(const unsigned char **s)
{
    const unsigned char *c = *s;
    uint32_t cp;
    int extra;
    if (c[0] < 0x80) { cp = c[0]; extra = 0; }
    else if ((c[0] & 0xE0) == 0xC0) { cp = c[0] & 0x1F; extra = 1; }
    else if ((c[0] & 0xF0) == 0xE0) { cp = c[0] & 0x0F; extra = 2; }
    else if ((c[0] & 0xF8) == 0xF0) { cp = c[0] & 0x07; extra = 3; }
    else { *s = c + 1; return 0xFFFD; }
    for (int i = 1; i <= extra; i++) {
        if ((c[i] & 0xC0) != 0x80) {
            *s = c + i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (c[i] & 0x3F);
    }
    *s = c + extra + 1;
    return cp;
}

/* String16: int32 length in UTF-16 units, the units, a NUL unit, padded to 4. */
void wdb_put_str16(struct wdb_parcel *p, const char *utf8)
{
    if (utf8 == NULL) {
        wdb_put_i32(p, -1);
        return;
    }
    size_t units = 0;
    for (const unsigned char *s = (const unsigned char *)utf8; *s != 0;) {
        units += utf8_next(&s) >= 0x10000 ? 2 : 1;
    }
    if (units > WDB_MAX_REPLY / 4) {
        p->error = WDB_EINVAL;
        return;
    }
    wdb_put_i32(p, (int32_t)units);
    size_t bytes = ((units + 1) * 2 + 3) & ~(size_t)3;
    uint8_t *at = parcel_grow(p, bytes);
    if (at == NULL) {
        return;
    }
    for (const unsigned char *s = (const unsigned char *)utf8; *s != 0;) {
        uint32_t cp = utf8_next(&s);
        uint16_t u[2];
        int n = 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u[0] = (uint16_t)(0xD800 | (cp >> 10));
            u[1] = (uint16_t)(0xDC00 | (cp & 0x3FF));
            n = 2;
        } else {
            u[0] = (uint16_t)cp;
        }
        memcpy(at, u, (size_t)n * 2);
        at += n * 2;
    }
}

void wdb_put_token(struct wdb_parcel *p, const char *interface)
{
    wdb_put_i32(p, (int32_t)0x80000000);   /* strict-mode policy | "has RPC header" */
    wdb_put_i32(p, -1);                    /* work source uid: unset */
    wdb_put_i32(p, 0x53595354);            /* 'SYST' */
    wdb_put_str16(p, interface);
}

int32_t wdb_get_i32(struct wdb_parcel *p)
{
    int32_t v = 0;
    if (p->error) {
        return 0;
    }
    if (p->pos + 4 > p->len) {
        p->error = WDB_EPROTO;
        return 0;
    }
    memcpy(&v, p->data + p->pos, 4);
    p->pos += 4;
    return v;
}

char *wdb_get_str16(struct wdb_parcel *p)
{
    int32_t units = wdb_get_i32(p);
    if (p->error || units < 0) {
        return NULL;                        /* null string, or error */
    }
    size_t bytes = (((size_t)units + 1) * 2 + 3) & ~(size_t)3;
    if ((size_t)units > WDB_MAX_REPLY || p->pos + bytes > p->len) {
        p->error = WDB_EPROTO;
        return NULL;
    }
    char *out = malloc((size_t)units * 3 + 1);      /* worst case: 3 bytes/unit */
    if (out == NULL) {
        p->error = WDB_ENOMEM;
        return NULL;
    }
    const uint8_t *in = p->data + p->pos;
    size_t o = 0;
    for (int32_t i = 0; i < units; i++) {
        uint16_t u;
        memcpy(&u, in + 2 * (size_t)i, 2);
        uint32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units) {
            uint16_t lo;
            memcpy(&lo, in + 2 * (size_t)(i + 1), 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)u - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            /* a surrogate pair is 2 units = 6 bytes of budget, 4 used */
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    p->pos += bytes;
    return out;
}

/* ---- connection ------------------------------------------------------------ */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct wdb *wdb_new(const char *binder_path)
{
    struct wdb *b = calloc(1, sizeof *b);
    if (b == NULL) {
        return NULL;
    }
    b->fd = -1;
    if (binder_path != NULL) {
        strncpy(b->path, binder_path, sizeof b->path - 1);
    }
    return b;
}

void wdb_reset(struct wdb *b)
{
    if (b == NULL) {
        return;
    }
    if (b->map != NULL && b->map != MAP_FAILED) {
        munmap(b->map, WDB_MAP_SIZE);
    }
    b->map = NULL;
    if (b->fd >= 0) {
        close(b->fd);       /* binder_release: refs, buffers, in-flight work */
    }
    b->fd = -1;
    b->have_platform = 0;
}

void wdb_free(struct wdb *b)
{
    wdb_reset(b);
    free(b);
}

void wdb_set_path(struct wdb *b, const char *binder_path)
{
    if (b == NULL || binder_path == NULL || strcmp(b->path, binder_path) == 0) {
        return;
    }
    wdb_reset(b);
    strncpy(b->path, binder_path, sizeof b->path - 1);
    b->path[sizeof b->path - 1] = 0;
}

static int wdb_connect(struct wdb *b)
{
    if (b->fd >= 0) {
        return WDB_OK;
    }
    if (b->path[0] == 0) {
        return WDB_EOPEN;
    }
    int fd = open(b->path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return WDB_EOPEN;
    }
    struct binder_version v;
    if (ioctl(fd, BINDER_VERSION, &v) < 0 ||
        v.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION) {
        close(fd);
        return WDB_EPROTO;
    }
    void *map = mmap(NULL, WDB_MAP_SIZE, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return WDB_EOPEN;
    }
    uint32_t max_threads = 0;       /* pure client: never ask us to spawn loopers */
    (void)ioctl(fd, BINDER_SET_MAX_THREADS, &max_threads);
    b->fd = fd;
    b->map = map;
    return WDB_OK;
}

static int bwr_write(struct wdb *b, void *buf, size_t n)
{
    struct binder_write_read bwr;
    memset(&bwr, 0, sizeof bwr);
    bwr.write_size = n;
    bwr.write_buffer = (binder_uintptr_t)(uintptr_t)buf;
    return ioctl(b->fd, BINDER_WRITE_READ, &bwr) < 0 ? -1 : 0;
}

static void free_buffer(struct wdb *b, binder_uintptr_t ptr)
{
    struct {
        uint32_t cmd;
        binder_uintptr_t ptr;
    } __attribute__((packed)) f = { BC_FREE_BUFFER, ptr };
    (void)bwr_write(b, &f, sizeof f);
}

/*
 * `object`, when non-NULL, receives the first binder handle in the reply
 * (servicemanager lookups).  It is referenced BEFORE the reply buffer is
 * freed: the buffer holds the only reference the kernel took on our behalf,
 * and a handle used after that comes back BR_FAILED_REPLY.
 */
static int transact(struct wdb *b, uint32_t handle, uint32_t code,
                    const struct wdb_parcel *req, struct wdb_parcel *reply,
                    int timeout_ms, uint32_t *object, int *have_object)
{
    wdb_parcel_init(reply);
    if (have_object != NULL) {
        *have_object = 0;
    }
    if (req == NULL || req->error) {
        return req != NULL ? req->error : WDB_EINVAL;
    }
    int rc = wdb_connect(b);
    if (rc != WDB_OK) {
        return rc;
    }

    struct {
        uint32_t cmd;
        struct binder_transaction_data t;
    } __attribute__((packed)) w;
    memset(&w, 0, sizeof w);
    w.cmd = BC_TRANSACTION;
    w.t.target.handle = handle;
    w.t.code = code;
    w.t.flags = TF_ACCEPT_FDS;
    w.t.data_size = req->len;
    w.t.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)req->data;
    if (bwr_write(b, &w, sizeof w) < 0) {
        wdb_reset(b);
        return WDB_EDEAD;
    }

    const int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 1);
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            wdb_reset(b);           /* discards the in-flight transaction */
            return WDB_ETIMEDOUT;
        }
        struct pollfd pfd = { .fd = b->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)left);
        if (pr < 0 && errno == EINTR) {
            continue;
        }
        if (pr <= 0) {
            wdb_reset(b);
            return pr == 0 ? WDB_ETIMEDOUT : WDB_EDEAD;
        }

        uint8_t rb[512];
        struct binder_write_read bwr;
        memset(&bwr, 0, sizeof bwr);
        bwr.read_size = sizeof rb;
        bwr.read_buffer = (binder_uintptr_t)(uintptr_t)rb;
        if (ioctl(b->fd, BINDER_WRITE_READ, &bwr) < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            wdb_reset(b);
            return WDB_EDEAD;
        }

        size_t p = 0;
        while (p + 4 <= bwr.read_consumed) {
            uint32_t cmd;
            memcpy(&cmd, rb + p, 4);
            p += 4;
            size_t payload = _IOC_SIZE(cmd);
            if (p + payload > bwr.read_consumed) {
                wdb_reset(b);
                return WDB_EPROTO;
            }
            if (cmd == BR_DEAD_REPLY) {
                wdb_reset(b);
                return WDB_EDEAD;
            }
            if (cmd == BR_FAILED_REPLY || cmd == BR_FROZEN_REPLY) {
                wdb_reset(b);
                return WDB_EPROTO;
            }
            if (cmd == BR_TRANSACTION || cmd == BR_TRANSACTION_SEC_CTX) {
                /* We publish nothing, so nobody can call us — but never leak
                 * a kernel buffer. */
                struct binder_transaction_data t;
                memcpy(&t, rb + p, sizeof t);
                free_buffer(b, t.data.ptr.buffer);
            } else if (cmd == BR_REPLY) {
                struct binder_transaction_data t;
                memcpy(&t, rb + p, sizeof t);
                const uint8_t *data = (const uint8_t *)(uintptr_t)t.data.ptr.buffer;
                int result = WDB_OK;
                if (t.data_size > WDB_MAX_REPLY) {
                    result = WDB_EPROTO;
                } else if (t.data_size > 0) {
                    uint8_t *at = parcel_grow(reply, (size_t)t.data_size);
                    if (at == NULL) {
                        result = WDB_ENOMEM;
                    } else {
                        memcpy(at, data, (size_t)t.data_size);
                    }
                }
                if (result == WDB_OK && object != NULL &&
                    t.offsets_size >= sizeof(binder_size_t)) {
                    binder_size_t off;
                    memcpy(&off, (const void *)(uintptr_t)t.data.ptr.offsets, sizeof off);
                    if (off + sizeof(struct flat_binder_object) <= t.data_size) {
                        struct flat_binder_object fo;
                        memcpy(&fo, data + off, sizeof fo);
                        if (fo.hdr.type == BINDER_TYPE_HANDLE) {
                            struct {
                                uint32_t c1, h1, c2, h2;
                            } __attribute__((packed)) refs =
                                { BC_INCREFS, fo.handle, BC_ACQUIRE, fo.handle };
                            (void)bwr_write(b, &refs, sizeof refs);
                            *object = fo.handle;
                            *have_object = 1;
                        }
                    }
                }
                free_buffer(b, t.data.ptr.buffer);
                if (result != WDB_OK) {
                    wdb_parcel_free(reply);
                    return result;
                }
                /* The driver answers for the remote when it could not deliver
                 * (e.g. unknown code): a bare status_t, flagged. */
                if (t.flags & TF_STATUS_CODE) {
                    wdb_parcel_free(reply);
                    return WDB_EPROTO;
                }
                return WDB_OK;
            }
            /* BR_NOOP, BR_TRANSACTION_COMPLETE, BR_SPAWN_LOOPER, the ref
             * count commands, ...: nothing to do. */
            p += payload;
        }
    }
}

int wdb_transact(struct wdb *b, uint32_t handle, uint32_t code,
                 const struct wdb_parcel *request, struct wdb_parcel *reply, int timeout_ms)
{
    if (b == NULL || reply == NULL) {
        return WDB_EINVAL;
    }
    return transact(b, handle, code, request, reply, timeout_ms, NULL, NULL);
}

int wdb_get_service(struct wdb *b, const char *name, int timeout_ms, uint32_t *handle)
{
    if (b == NULL || name == NULL || handle == NULL) {
        return WDB_EINVAL;
    }
    struct wdb_parcel req, rep;
    wdb_parcel_init(&req);
    wdb_put_token(&req, IFACE_SERVICE_MANAGER);
    wdb_put_str16(&req, name);
    int have = 0;
    int rc = transact(b, 0, SM_LOOKUP, &req, &rep, timeout_ms, handle, &have);
    wdb_parcel_free(&req);
    wdb_parcel_free(&rep);
    if (rc != WDB_OK) {
        return rc;
    }
    return have ? WDB_OK : WDB_ENOSERVICE;
}

/* ---- IPlatform ---------------------------------------------------------------- */

/* Request = token + whatever `fill` appended; reply is left positioned after a
 * zero exception code. */
static int platform_call(struct wdb *b, uint32_t code, struct wdb_parcel *req,
                         struct wdb_parcel *rep, int timeout_ms)
{
    wdb_parcel_init(rep);
    if (b == NULL) {
        wdb_parcel_free(req);
        return WDB_EINVAL;
    }
    const int64_t deadline = now_ms() + timeout_ms;
    int rc = WDB_OK;
    /* One retry: a cached handle from before Android's framework restarted
     * (zygote death inside a living container) answers DEAD once. */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!b->have_platform) {
            int left = (int)(deadline - now_ms());
            rc = wdb_get_service(b, SERVICE_PLATFORM, left > 0 ? left : 1, &b->platform);
            if (rc != WDB_OK) {
                break;
            }
            b->have_platform = 1;
        }
        int left = (int)(deadline - now_ms());
        rc = transact(b, b->platform, code, req, rep, left > 0 ? left : 1, NULL, NULL);
        if (rc != WDB_EDEAD) {
            break;
        }
        wdb_parcel_free(rep);
    }
    wdb_parcel_free(req);
    if (rc != WDB_OK) {
        return rc;
    }
    int32_t ex = wdb_get_i32(rep);
    if (rep->error) {
        wdb_parcel_free(rep);
        return WDB_EPROTO;
    }
    if (ex != 0) {
        wdb_parcel_free(rep);
        return WDB_EREMOTE;
    }
    return WDB_OK;
}

static void platform_request(struct wdb_parcel *req)
{
    wdb_parcel_init(req);
    wdb_put_token(req, IFACE_PLATFORM);
}

static int finish(struct wdb_parcel *rep)
{
    int rc = rep->error ? WDB_EPROTO : WDB_OK;
    wdb_parcel_free(rep);
    return rc;
}

int wdb_platform_getprop(struct wdb *b, const char *key, const char *dflt, char **value,
                         int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_str16(&req, key);
    wdb_put_str16(&req, dflt != NULL ? dflt : "");
    int rc = platform_call(b, PLATFORM_GETPROP, &req, &rep, timeout_ms);
    if (rc != WDB_OK) {
        return rc;
    }
    *value = wdb_get_str16(&rep);
    return finish(&rep);
}

int wdb_platform_setprop(struct wdb *b, const char *key, const char *value, int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_str16(&req, key);
    wdb_put_str16(&req, value);
    int rc = platform_call(b, PLATFORM_SETPROP, &req, &rep, timeout_ms);
    return rc != WDB_OK ? rc : finish(&rep);
}

void wdb_platform_free_apps(struct wdb_app *apps, size_t count)
{
    if (apps == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(apps[i].name);
        free(apps[i].package);
        free(apps[i].activity);
    }
    free(apps);
}

/*
 * AppInfo on the wire: int32 non-null marker, then name, packageName, action,
 * launchIntent, componentPackageName, componentClassName (String16 each) and
 * a String16 list of categories.
 */
int wdb_platform_list_apps(struct wdb *b, struct wdb_app **apps, size_t *count, int timeout_ms)
{
    struct wdb_parcel req, rep;
    *apps = NULL;
    *count = 0;
    platform_request(&req);
    int rc = platform_call(b, PLATFORM_GET_APPS_INFO, &req, &rep, timeout_ms);
    if (rc != WDB_OK) {
        return rc;
    }
    int32_t n = wdb_get_i32(&rep);
    if (rep.error || n < 0 || n > WDB_MAX_LIST) {
        wdb_parcel_free(&rep);
        return WDB_EPROTO;
    }
    struct wdb_app *out = calloc((size_t)n > 0 ? (size_t)n : 1, sizeof *out);
    if (out == NULL) {
        wdb_parcel_free(&rep);
        return WDB_ENOMEM;
    }
    size_t used = 0;
    for (int32_t i = 0; i < n && !rep.error; i++) {
        if (wdb_get_i32(&rep) == 0) {
            continue;                               /* null entry */
        }
        struct wdb_app *a = &out[used];
        a->name = wdb_get_str16(&rep);
        a->package = wdb_get_str16(&rep);
        free(wdb_get_str16(&rep));                  /* action */
        free(wdb_get_str16(&rep));                  /* launchIntent */
        free(wdb_get_str16(&rep));                  /* componentPackageName */
        a->activity = wdb_get_str16(&rep);
        int32_t cats = wdb_get_i32(&rep);
        if (cats > WDB_MAX_LIST) {
            rep.error = WDB_EPROTO;
        }
        for (int32_t k = 0; k < cats && !rep.error; k++) {
            free(wdb_get_str16(&rep));
        }
        if (!rep.error && a->package != NULL) {
            used++;
        } else {
            free(a->name);
            free(a->package);
            free(a->activity);
            memset(a, 0, sizeof *a);
        }
    }
    if (rep.error) {
        wdb_platform_free_apps(out, used);
        wdb_parcel_free(&rep);
        return WDB_EPROTO;
    }
    wdb_parcel_free(&rep);
    *apps = out;
    *count = used;
    return WDB_OK;
}

static int call_str_to_int(struct wdb *b, uint32_t code, const char *arg, int32_t *result,
                           int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_str16(&req, arg);
    int rc = platform_call(b, code, &req, &rep, timeout_ms);
    if (rc != WDB_OK) {
        return rc;
    }
    int32_t v = wdb_get_i32(&rep);
    if (result != NULL) {
        *result = v;
    }
    return finish(&rep);
}

int wdb_platform_install_app(struct wdb *b, const char *path_in_container, int32_t *result,
                             int timeout_ms)
{
    return call_str_to_int(b, PLATFORM_INSTALL_APP, path_in_container, result, timeout_ms);
}

int wdb_platform_remove_app(struct wdb *b, const char *package, int32_t *result, int timeout_ms)
{
    return call_str_to_int(b, PLATFORM_REMOVE_APP, package, result, timeout_ms);
}

int wdb_platform_launch_app(struct wdb *b, const char *package, int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_str16(&req, package);
    int rc = platform_call(b, PLATFORM_LAUNCH_APP, &req, &rep, timeout_ms);
    return rc != WDB_OK ? rc : finish(&rep);
}

int wdb_platform_app_name(struct wdb *b, const char *package, char **name, int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_str16(&req, package);
    int rc = platform_call(b, PLATFORM_GET_APP_NAME, &req, &rep, timeout_ms);
    if (rc != WDB_OK) {
        return rc;
    }
    *name = wdb_get_str16(&rep);
    return finish(&rep);
}

int wdb_platform_settings_put_string(struct wdb *b, int ns, const char *key, const char *value,
                                     int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_i32(&req, ns);
    wdb_put_str16(&req, key);
    wdb_put_str16(&req, value);
    int rc = platform_call(b, PLATFORM_SETTINGS_PUT_STRING, &req, &rep, timeout_ms);
    return rc != WDB_OK ? rc : finish(&rep);
}

int wdb_platform_settings_get_string(struct wdb *b, int ns, const char *key, char **value,
                                     int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_i32(&req, ns);
    wdb_put_str16(&req, key);
    int rc = platform_call(b, PLATFORM_SETTINGS_GET_STRING, &req, &rep, timeout_ms);
    if (rc != WDB_OK) {
        return rc;
    }
    *value = wdb_get_str16(&rep);
    return finish(&rep);
}

int wdb_platform_settings_put_int(struct wdb *b, int ns, const char *key, int32_t value,
                                  int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_i32(&req, ns);
    wdb_put_str16(&req, key);
    wdb_put_i32(&req, value);
    int rc = platform_call(b, PLATFORM_SETTINGS_PUT_INT, &req, &rep, timeout_ms);
    return rc != WDB_OK ? rc : finish(&rep);
}

int wdb_platform_settings_get_int(struct wdb *b, int ns, const char *key, int32_t *value,
                                  int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_i32(&req, ns);
    wdb_put_str16(&req, key);
    int rc = platform_call(b, PLATFORM_SETTINGS_GET_INT, &req, &rep, timeout_ms);
    if (rc != WDB_OK) {
        return rc;
    }
    *value = wdb_get_i32(&rep);
    return finish(&rep);
}

int wdb_platform_launch_intent(struct wdb *b, const char *action, const char *uri,
                               int timeout_ms)
{
    struct wdb_parcel req, rep;
    platform_request(&req);
    wdb_put_str16(&req, action);
    wdb_put_str16(&req, uri);
    int rc = platform_call(b, PLATFORM_LAUNCH_INTENT, &req, &rep, timeout_ms);
    return rc != WDB_OK ? rc : finish(&rep);
}
