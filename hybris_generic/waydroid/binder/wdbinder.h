/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * libwdbinder — a libc-only Android binder client for the OHOS host.
 *
 * It talks to the Waydroid container's servicemanager through the container's
 * own /dev/binder — reached from the host as
 * /proc/<container init pid>/root/dev/binder — and from there to
 * lineageos.waydroid.IPlatform ("waydroidplatform"): the app list, launching,
 * properties, settings.  No Android library, no nsenter; the wire format is
 * written out by hand and documented in
 * docs/hybris_generic/android_app_launcher_plan.md §1.1.
 *
 * Two rules shape the API:
 *
 *  - Every call has a DEADLINE.  The container can be frozen (cgroup freezer)
 *    or wedged, and a binder transaction into a stopped process blocks until
 *    it runs again.  The fd is non-blocking and replies are awaited with
 *    poll(); when the deadline passes the connection is CLOSED — that is what
 *    makes the kernel discard the in-flight transaction, so a late reply can
 *    never be mistaken for the next call's — and the call returns
 *    WDB_ETIMEDOUT.  The next call reconnects by itself.
 *
 *  - A connection is NOT thread-safe.  Callers serialize (the compositor
 *    keeps one behind a mutex; calls take milliseconds).
 *
 * Plain C on purpose: it must stay usable from a future broker process, from
 * tools, and on a non-OHOS host.
 */

#ifndef WAYDROID_WDBINDER_H
#define WAYDROID_WDBINDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WDB_OK = 0,
    WDB_EOPEN = -1,        /* no binder node: container not running */
    WDB_ENOSERVICE = -2,   /* servicemanager up, service not registered (yet) */
    WDB_ETIMEDOUT = -3,    /* deadline passed; connection was reset */
    WDB_EDEAD = -4,        /* the remote died; connection was reset */
    WDB_EPROTO = -5,       /* malformed or unexpected reply */
    WDB_EREMOTE = -6,      /* the service threw (exception code in *ex) */
    WDB_ENOMEM = -7,
    WDB_EINVAL = -8,
};

const char *wdb_strerror(int err);

/* ---- parcel ------------------------------------------------------------ */

struct wdb_parcel {
    uint8_t *data;
    size_t len;         /* bytes used (write) / available (read) */
    size_t cap;
    size_t pos;         /* read cursor */
    int error;          /* sticky: any failed read/write sets it */
};

void wdb_parcel_init(struct wdb_parcel *p);
void wdb_parcel_free(struct wdb_parcel *p);

void wdb_put_i32(struct wdb_parcel *p, int32_t v);
/* UTF-8 in, String16 on the wire.  NULL writes a null string (-1). */
void wdb_put_str16(struct wdb_parcel *p, const char *utf8);
/* The Android 11+ RPC header every AIDL request starts with. */
void wdb_put_token(struct wdb_parcel *p, const char *interface);

int32_t wdb_get_i32(struct wdb_parcel *p);
/* String16 off the wire, UTF-8 out (malloc'd; NULL for a null string or on
 * error — check p->error to tell them apart).  Caller frees. */
char *wdb_get_str16(struct wdb_parcel *p);

/* ---- connection ---------------------------------------------------------- */

struct wdb;

/* Does not touch the device yet; the first call connects. */
struct wdb *wdb_new(const char *binder_path);
void wdb_free(struct wdb *b);
/* Point at another node (a new container generation) and drop the old
 * connection.  No-op if the path is unchanged. */
void wdb_set_path(struct wdb *b, const char *binder_path);
/* Drop the connection and every cached handle. */
void wdb_reset(struct wdb *b);

/* servicemanager lookup; the handle stays valid until wdb_reset(). */
int wdb_get_service(struct wdb *b, const char *name, int timeout_ms, uint32_t *handle);

/* One synchronous transaction.  `reply` is initialised here and owns a copy
 * of the reply data (wdb_parcel_free it), positioned at its start. */
int wdb_transact(struct wdb *b, uint32_t handle, uint32_t code,
                 const struct wdb_parcel *request, struct wdb_parcel *reply,
                 int timeout_ms);

/* ---- lineageos.waydroid.IPlatform ------------------------------------------ */

struct wdb_app {
    char *name;             /* label, UTF-8 */
    char *package;
    char *activity;         /* launcher activity class */
};

enum wdb_settings_ns {      /* IPlatform's "mode" argument */
    WDB_SETTINGS_SECURE = 0,
    WDB_SETTINGS_SYSTEM = 1,
    WDB_SETTINGS_GLOBAL = 2,
};

/* All return WDB_*; *ex (optional) receives the Java exception code when the
 * result is WDB_EREMOTE.  Strings returned through char** are malloc'd. */
int wdb_platform_getprop(struct wdb *b, const char *key, const char *dflt,
                         char **value, int timeout_ms);
int wdb_platform_setprop(struct wdb *b, const char *key, const char *value, int timeout_ms);
int wdb_platform_list_apps(struct wdb *b, struct wdb_app **apps, size_t *count, int timeout_ms);
void wdb_platform_free_apps(struct wdb_app *apps, size_t count);
int wdb_platform_install_app(struct wdb *b, const char *path_in_container, int32_t *result,
                             int timeout_ms);
int wdb_platform_remove_app(struct wdb *b, const char *package, int32_t *result, int timeout_ms);
int wdb_platform_launch_app(struct wdb *b, const char *package, int timeout_ms);
int wdb_platform_app_name(struct wdb *b, const char *package, char **name, int timeout_ms);
int wdb_platform_settings_put_string(struct wdb *b, int ns, const char *key, const char *value,
                                     int timeout_ms);
int wdb_platform_settings_get_string(struct wdb *b, int ns, const char *key, char **value,
                                     int timeout_ms);
int wdb_platform_settings_put_int(struct wdb *b, int ns, const char *key, int32_t value,
                                  int timeout_ms);
int wdb_platform_settings_get_int(struct wdb *b, int ns, const char *key, int32_t *value,
                                  int timeout_ms);
int wdb_platform_launch_intent(struct wdb *b, const char *action, const char *uri,
                               int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WAYDROID_WDBINDER_H */
