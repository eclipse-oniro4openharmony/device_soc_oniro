/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * emulator_touch: make clicks in the emulator window act as touches.
 *
 * run.sh gives the guest QEMU's absolute pointer (virtio-tablet), so QEMU never
 * grabs the host mouse and window coordinates map 1:1 onto the screen. The
 * input service classifies that device as a mouse (absolute axes plus mouse
 * buttons): it draws a cursor and turns swipes into mouse drags, which the
 * phone UI's gestures do not follow. This daemon grabs the tablet exclusively
 * and re-emits it through /dev/uinput as a single-finger direct-touch device
 * with the same axis ranges:
 *
 *   left button down / move / up  ->  finger down / move / up
 *   right button                  ->  Back key
 *
 * Pointer motion with no button held is dropped (a finger cannot hover).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD002F01
#define LOG_TAG "EmulatorTouch"

#define TOUCH_NAME "Oniro Emulator Touchscreen"
#define FIND_TRIES 60          /* the tablet is normally there at boot */
#define FIND_INTERVAL_US 500000
#define EVENT_BATCH 64
#define TRACKING_ID_MAX 65535

/* Names QEMU gives its absolute pointers (virtio-tablet-pci, usb-tablet). */
static const char *const TABLET_NAMES[] = {
    "QEMU Virtio Tablet",
    "QEMU QEMU USB Tablet",
};

struct TouchState {
    int x;
    int y;
    bool down;        /* finger currently down on the uinput device */
    bool wantDown;    /* left button state as of the current frame */
    bool moved;       /* x/y changed in the current frame */
    int back;         /* -1: no change, 0/1: right button released/pressed */
    int trackingId;
};

static bool IsTablet(const char *name)
{
    for (size_t i = 0; i < sizeof(TABLET_NAMES) / sizeof(TABLET_NAMES[0]); i++) {
        if (strcmp(name, TABLET_NAMES[i]) == 0) {
            return true;
        }
    }
    return false;
}

static int OpenTablet(struct input_absinfo *absX, struct input_absinfo *absY)
{
    DIR *dir = opendir("/dev/input");
    if (dir == NULL) {
        return -1;
    }
    int found = -1;
    struct dirent *entry;
    while (found < 0 && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", strlen("event")) != 0) {
            continue;
        }
        char path[64];
        char name[128] = { 0 };
        (void)snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && IsTablet(name) &&
            ioctl(fd, EVIOCGABS(ABS_X), absX) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), absY) == 0) {
            HILOG_INFO(LOG_CORE, "tablet %{public}s is %{public}s, x %{public}d..%{public}d, y %{public}d..%{public}d",
                name, path, absX->minimum, absX->maximum, absY->minimum, absY->maximum);
            found = fd;
        } else {
            close(fd);
        }
    }
    closedir(dir);
    return found;
}

static int SetAbs(int fd, unsigned int code, int minimum, int maximum)
{
    struct uinput_abs_setup abs;
    (void)memset(&abs, 0, sizeof(abs));
    abs.code = code;
    abs.absinfo.minimum = minimum;
    abs.absinfo.maximum = maximum;
    if (ioctl(fd, UI_SET_ABSBIT, code) < 0 || ioctl(fd, UI_ABS_SETUP, &abs) < 0) {
        return -1;
    }
    return 0;
}

static int CreateTouchscreen(const struct input_absinfo *absX, const struct input_absinfo *absY)
{
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        HILOG_ERROR(LOG_CORE, "open /dev/uinput failed: %{public}s", strerror(errno));
        return -1;
    }
    int ret = 0;
    ret |= ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ret |= ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ret |= ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ret |= ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ret |= ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    ret |= ioctl(fd, UI_SET_KEYBIT, KEY_BACK);
    ret |= ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    ret |= SetAbs(fd, ABS_X, absX->minimum, absX->maximum);
    ret |= SetAbs(fd, ABS_Y, absY->minimum, absY->maximum);
    ret |= SetAbs(fd, ABS_MT_SLOT, 0, 0);
    ret |= SetAbs(fd, ABS_MT_TRACKING_ID, 0, TRACKING_ID_MAX);
    ret |= SetAbs(fd, ABS_MT_POSITION_X, absX->minimum, absX->maximum);
    ret |= SetAbs(fd, ABS_MT_POSITION_Y, absY->minimum, absY->maximum);

    struct uinput_setup setup;
    (void)memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_VIRTUAL;
    (void)strncpy(setup.name, TOUCH_NAME, UINPUT_MAX_NAME_SIZE - 1);
    ret |= ioctl(fd, UI_DEV_SETUP, &setup);
    ret |= ioctl(fd, UI_DEV_CREATE);
    if (ret < 0) {
        HILOG_ERROR(LOG_CORE, "uinput setup failed: %{public}s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void Emit(int fd, unsigned short type, unsigned short code, int value)
{
    struct input_event ev;
    (void)memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        HILOG_ERROR(LOG_CORE, "uinput write failed: %{public}s", strerror(errno));
    }
}

static void EmitPosition(int fd, const struct TouchState *st)
{
    Emit(fd, EV_ABS, ABS_MT_POSITION_X, st->x);
    Emit(fd, EV_ABS, ABS_MT_POSITION_Y, st->y);
    Emit(fd, EV_ABS, ABS_X, st->x);
    Emit(fd, EV_ABS, ABS_Y, st->y);
}

/* Turn one tablet frame (up to SYN_REPORT) into one touchscreen frame. */
static void Flush(int fd, struct TouchState *st)
{
    bool sync = false;
    if (st->wantDown && !st->down) {
        st->trackingId = (st->trackingId + 1) % (TRACKING_ID_MAX + 1);
        Emit(fd, EV_ABS, ABS_MT_TRACKING_ID, st->trackingId);
        EmitPosition(fd, st);
        Emit(fd, EV_KEY, BTN_TOUCH, 1);
        Emit(fd, EV_KEY, BTN_TOOL_FINGER, 1);
        st->down = true;
        sync = true;
    } else if (st->down && !st->wantDown) {
        if (st->moved) {
            EmitPosition(fd, st);
        }
        Emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        Emit(fd, EV_KEY, BTN_TOUCH, 0);
        Emit(fd, EV_KEY, BTN_TOOL_FINGER, 0);
        st->down = false;
        sync = true;
    } else if (st->down && st->moved) {
        EmitPosition(fd, st);
        sync = true;
    }
    if (st->back >= 0) {
        Emit(fd, EV_KEY, KEY_BACK, st->back);
        st->back = -1;
        sync = true;
    }
    if (sync) {
        Emit(fd, EV_SYN, SYN_REPORT, 0);
    }
    st->moved = false;
}

static void Handle(int touchFd, struct TouchState *st, const struct input_event *ev)
{
    switch (ev->type) {
        case EV_ABS:
            if (ev->code == ABS_X) {
                st->x = ev->value;
                st->moved = true;
            } else if (ev->code == ABS_Y) {
                st->y = ev->value;
                st->moved = true;
            }
            break;
        case EV_KEY:
            if (ev->code == BTN_LEFT) {
                st->wantDown = (ev->value != 0);
            } else if (ev->code == BTN_RIGHT && ev->value != 2) {   /* 2 = autorepeat */
                st->back = (ev->value != 0);
            }
            break;
        case EV_SYN:
            if (ev->code == SYN_REPORT) {
                Flush(touchFd, st);
            }
            break;
        default:
            break;
    }
}

int main(void)
{
    struct input_absinfo absX;
    struct input_absinfo absY;
    int tabletFd = -1;
    for (int i = 0; i < FIND_TRIES && tabletFd < 0; i++) {
        tabletFd = OpenTablet(&absX, &absY);
        if (tabletFd < 0) {
            usleep(FIND_INTERVAL_US);
        }
    }
    if (tabletFd < 0) {
        /* QEMU was started without -device virtio-tablet-pci: nothing to do. */
        HILOG_INFO(LOG_CORE, "no QEMU tablet, exiting");
        return 0;
    }

    int touchFd = CreateTouchscreen(&absX, &absY);
    if (touchFd < 0) {
        close(tabletFd);
        return 1;
    }
    /* Exclusive: the input service no longer sees the tablet as a mouse. */
    if (ioctl(tabletFd, EVIOCGRAB, 1) < 0) {
        HILOG_ERROR(LOG_CORE, "EVIOCGRAB failed: %{public}s", strerror(errno));
    }
    HILOG_INFO(LOG_CORE, "clicks on the tablet now drive \"%{public}s\"", TOUCH_NAME);

    struct TouchState st;
    (void)memset(&st, 0, sizeof(st));
    st.back = -1;
    st.trackingId = -1;
    struct input_event events[EVENT_BATCH];
    for (;;) {
        ssize_t n = read(tabletFd, events, sizeof(events));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            HILOG_ERROR(LOG_CORE, "tablet read failed: %{public}s", strerror(errno));
            break;
        }
        for (size_t i = 0; i < (size_t)n / sizeof(events[0]); i++) {
            Handle(touchFd, &st, &events[i]);
        }
    }
    ioctl(touchFd, UI_DEV_DESTROY);
    close(touchFd);
    close(tabletFd);
    return 1;
}
