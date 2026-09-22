/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * wdctl — talk to the running Android container from an OHOS root shell.
 * A thin front for libwdbinder (wdbinder.h); replaces the nsenter recipes.
 *
 *   wdctl list                          installed launcher apps
 *   wdctl launch <package>
 *   wdctl name <package>
 *   wdctl icon <package> <file>         copy the app's icon PNG out
 *   wdctl getprop <key> [default]
 *   wdctl setprop <key> <value>
 *   wdctl settings get|put secure|system|global <key> [value]      (strings)
 *   wdctl settings geti|puti secure|system|global <key> [int]
 *   wdctl install <path inside the container>
 *   wdctl remove <package>
 *   wdctl intent <action> <uri>
 *
 * -t <ms> sets the deadline (default 5000).  The container must be running
 * and THAWED: a call into a frozen container times out by design.
 *
 * The binder node is the container's own, reached through its root:
 * /proc/<pid in /data/waydroid/container.pid>/root/dev/binder ($WDBINDER
 * overrides).  Root only — that is the point of the private binderfs.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wdbinder.h"

#define PID_FILE "/data/waydroid/container.pid"

static int container_root(char *out, size_t n)
{
    FILE *f = fopen(PID_FILE, "re");
    long pid = 0;
    if (f == NULL) {
        return -1;
    }
    if (fscanf(f, "%ld", &pid) != 1) {
        pid = 0;
    }
    fclose(f);
    if (pid <= 0) {
        return -1;
    }
    snprintf(out, n, "/proc/%ld/root", pid);
    return 0;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int settings_ns(const char *s)
{
    if (strcmp(s, "secure") == 0) return WDB_SETTINGS_SECURE;
    if (strcmp(s, "system") == 0) return WDB_SETTINGS_SYSTEM;
    if (strcmp(s, "global") == 0) return WDB_SETTINGS_GLOBAL;
    return -1;
}

static int copy_icon(const char *root, const char *package, const char *dst)
{
    char src[512];
    if (strchr(package, '/') != NULL) {
        return -1;
    }
    snprintf(src, sizeof src, "%s/data/icons/%s.png", root, package);
    int in = open(src, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) {
        perror(src);
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        perror(dst);
        close(in);
        return -1;
    }
    char buf[8192];
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)r) != r) {
            r = -1;
            break;
        }
    }
    close(in);
    close(out);
    return r < 0 ? -1 : 0;
}

static int usage(void)
{
    fprintf(stderr,
        "usage: wdctl [-t ms] list | launch <pkg> | name <pkg> | icon <pkg> <file> |\n"
        "       getprop <key> [default] | setprop <key> <value> |\n"
        "       settings get|put|geti|puti secure|system|global <key> [value] |\n"
        "       install <path-in-container> | remove <pkg> | intent <action> <uri>\n");
    return 2;
}

int main(int argc, char **argv)
{
    int timeout = 5000;
    int a = 1;
    if (argc > 2 && strcmp(argv[1], "-t") == 0) {
        timeout = atoi(argv[2]);
        a = 3;
    }
    if (argc <= a) {
        return usage();
    }
    const char *cmd = argv[a];
    int nargs = argc - a - 1;
    char **args = argv + a + 1;

    char root[128] = "";
    char path[256];
    const char *env = getenv("WDBINDER");
    if (container_root(root, sizeof root) < 0 && env == NULL) {
        fprintf(stderr, "wdctl: no container (%s)\n", PID_FILE);
        return 1;
    }
    if (env != NULL) {
        snprintf(path, sizeof path, "%s", env);
    } else {
        snprintf(path, sizeof path, "%s/dev/binder", root);
    }

    if (strcmp(cmd, "icon") == 0 && nargs == 2) {
        return copy_icon(root, args[0], args[1]) < 0 ? 1 : 0;
    }

    struct wdb *b = wdb_new(path);
    if (b == NULL) {
        return 1;
    }
    double t0 = now_ms();
    int rc = WDB_EINVAL;
    if (strcmp(cmd, "list") == 0) {
        struct wdb_app *apps = NULL;
        size_t n = 0;
        rc = wdb_platform_list_apps(b, &apps, &n, timeout);
        for (size_t i = 0; rc == WDB_OK && i < n; i++) {
            printf("%-16s %-32s %s\n", apps[i].name ? apps[i].name : "",
                   apps[i].package, apps[i].activity ? apps[i].activity : "");
        }
        wdb_platform_free_apps(apps, n);
    } else if (strcmp(cmd, "launch") == 0 && nargs == 1) {
        rc = wdb_platform_launch_app(b, args[0], timeout);
    } else if (strcmp(cmd, "name") == 0 && nargs == 1) {
        char *s = NULL;
        rc = wdb_platform_app_name(b, args[0], &s, timeout);
        if (rc == WDB_OK) {
            printf("%s\n", s ? s : "");
        }
        free(s);
    } else if (strcmp(cmd, "getprop") == 0 && nargs >= 1) {
        char *s = NULL;
        rc = wdb_platform_getprop(b, args[0], nargs > 1 ? args[1] : "", &s, timeout);
        if (rc == WDB_OK) {
            printf("%s\n", s ? s : "");
        }
        free(s);
    } else if (strcmp(cmd, "setprop") == 0 && nargs == 2) {
        rc = wdb_platform_setprop(b, args[0], args[1], timeout);
    } else if (strcmp(cmd, "settings") == 0 && nargs >= 3 && settings_ns(args[1]) >= 0) {
        int ns = settings_ns(args[1]);
        if (strcmp(args[0], "get") == 0) {
            char *s = NULL;
            rc = wdb_platform_settings_get_string(b, ns, args[2], &s, timeout);
            if (rc == WDB_OK) {
                printf("%s\n", s ? s : "(null)");
            }
            free(s);
        } else if (strcmp(args[0], "geti") == 0) {
            int32_t v = 0;
            rc = wdb_platform_settings_get_int(b, ns, args[2], &v, timeout);
            if (rc == WDB_OK) {
                printf("%d\n", v);
            }
        } else if (strcmp(args[0], "put") == 0 && nargs == 4) {
            rc = wdb_platform_settings_put_string(b, ns, args[2], args[3], timeout);
        } else if (strcmp(args[0], "puti") == 0 && nargs == 4) {
            rc = wdb_platform_settings_put_int(b, ns, args[2], atoi(args[3]), timeout);
        }
    } else if (strcmp(cmd, "install") == 0 && nargs == 1) {
        int32_t r = 0;
        rc = wdb_platform_install_app(b, args[0], &r, timeout > 60000 ? timeout : 60000);
        if (rc == WDB_OK) {
            printf("%d\n", r);
        }
    } else if (strcmp(cmd, "remove") == 0 && nargs == 1) {
        int32_t r = 0;
        rc = wdb_platform_remove_app(b, args[0], &r, timeout > 60000 ? timeout : 60000);
        if (rc == WDB_OK) {
            printf("%d\n", r);
        }
    } else if (strcmp(cmd, "intent") == 0 && nargs == 2) {
        rc = wdb_platform_launch_intent(b, args[0], args[1], timeout);
    } else {
        wdb_free(b);
        return usage();
    }
    fprintf(stderr, "wdctl: %s: %s (%.1f ms)\n", cmd, wdb_strerror(rc), now_ms() - t0);
    wdb_free(b);
    return rc == WDB_OK ? 0 : 1;
}
