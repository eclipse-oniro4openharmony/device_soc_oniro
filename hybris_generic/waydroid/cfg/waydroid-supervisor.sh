#!/system/bin/sh
# Waydroid auto-start + crash-recovery supervisor (W5 lifecycle).
#
# Gating.  init can not gate us: a job hung on a boot stage (`boot`,
# `post-fs-data`) is matched by its NAME and its "condition" is ignored
# (trigger_manager.c GetBootCondition_), and a `param:` job does not fire for
# a persist param at boot — persist loading raises PARAM_WATCH, not the PARAM
# trigger.  So waydroid_compositor.cfg starts us unconditionally on those two
# stages and WE are the gate: if persist.waydroid.enabled is not 1 we ask init
# to stop us and are gone within a few seconds — on a device that never
# enabled waydroid (the X23: same image) nothing of the stack stays resident.
# The cfg's third job, `param:persist.waydroid.enabled=1`, does fire on a
# runtime `param set`, which is how enabling takes effect without a reboot.
# Disabling takes effect the same way: we notice within 2 s, tear the
# generation down (container, launcher, compositor) and stop.
#
# Start policy.  Enabled does not mean running:
#   persist.waydroid.autostart=1   bring the container up at boot, so the first
#                                  open is instant, and keep it (frozen while
#                                  hidden) — costs its RAM all the time;
#   anything else (the default)    ON DEMAND: nothing runs until the user opens
#                                  "Android Apps".  Its SHOW common event makes
#                                  samgr start this service (sa_profile/
#                                  9601.json), with "9601#..." as $1 — that
#                                  argument is how we tell a demand start from
#                                  init's boot-stage start.  After
#                                  persist.waydroid.idle_stop_s (default 1800,
#                                  0 = never) hidden, we tear down and stop
#                                  again; the next open starts over (~20 s).
#
# When enabled we bring up one container "generation" — a FRESH compositor
# (W3 rule: every new container generation must get a fresh compositor) plus
# the container via waydroidd — then block until the container init exits.
# When it does — Android powered off from inside the UI, a container crash,
# or an explicit teardown — the whole generation is torn down and a new one
# is started.  That gives the two things a bare `start waydroid_compositor`
# cannot:
#   * auto-start after a phone reboot, and
#   * recovery when the container dies for any reason (no more app stuck
#     forever at "Android is starting…").
#
# Code vs data.  Everything executable or configuring comes from the image
# (/system/bin/{waydroid_compositor,waydroidd}, /system/etc/waydroid/).
# /data/waydroid holds the two upstream images, data.img and runtime state;
# until the images are there we wait quietly.
#
# Only shell builtins + toybox applets known to be on the init PATH are used
# (cat/kill/killall/param/sleep/rm/ls/nohup/cd/mkdir/cp/chmod) — there is no
# awk/tr/date/grep.

DEMAND="$1"        # non-empty: started by samgr for a SHOW event
WD=/data/waydroid
ETC=/system/etc/waydroid
LOG="$WD/supervisor.log"
SOCK="$WD/run/xdg/wayland-0"

# Which binaries the next generation runs.  Iteration aid, debug images only:
# one dropped in $WD/bin wins.  Never on a production image — that would be
# root code from a writable partition.  Resolved per generation, so an override
# can be added or removed without restarting us.
pick_binaries() {
    COMPOSITOR=/system/bin/waydroid_compositor
    WAYDROIDD=/system/bin/waydroidd
    case "$(param get const.debuggable 2>/dev/null)" in
        1*)
            [ -x "$WD/bin/waydroid_compositor" ] && COMPOSITOR="$WD/bin/waydroid_compositor"
            [ -x "$WD/bin/waydroidd" ] && WAYDROIDD="$WD/bin/waydroidd"
            ;;
    esac
}

log() { echo "supervisor: $*" >>"$LOG" 2>/dev/null; }

# `param get` prints the value with a trailing space ("1 "), so glob-match
# rather than string-equal.  The param is only ever 0 or 1.
enabled() {
    case "$(param get persist.waydroid.enabled 2>/dev/null)" in
        1*) return 0 ;;
        *) return 1 ;;
    esac
}

teardown() {
    busy=0
    CPID=$(cat "$WD/container.pid" 2>/dev/null)
    [ -n "$CPID" ] && kill -9 "$CPID" 2>/dev/null && busy=1
    killall -9 waydroidd 2>/dev/null && busy=1
    killall -9 waydroid_compositor 2>/dev/null && busy=1
    param set waydroid.compositor.ready 0 2>/dev/null
    param set waydroid.compositor.frames 0 2>/dev/null
    # A dead compositor can not release the touch grab it took; never leave
    # it stranded over OHOS.
    param set waydroid.input.grab 0 2>/dev/null
    rm -f "$SOCK" 2>/dev/null
    # Clear the pid file: waydroidd rewrites it a beat AFTER launch, so if we
    # leave the previous generation's (now-dead) pid here, the monitor below
    # reads it, kill -0 fails at once, and we thrash a new generation every
    # couple of seconds instead of waiting for the real one.
    rm -f "$WD/container.pid" 2>/dev/null
    # Clear waydroidd's OWN self-lock too.  We just `killall -9`ed waydroidd,
    # and a -9 gives it no chance to unlink its pidfile — so waydroidd.pid is
    # left holding the dead instance's pid.  waydroidd's startup guard does a
    # bare `kill(pid, 0)`: harmless while that pid stays dead, but once the
    # kernel RECYCLES it onto some other long-lived process (seen in the wild:
    # /vendor/bin/gbe reusing it across a reboot), the guard false-positives
    # ("already running (pid N)"), waydroidd exits before starting the
    # container, container.pid never appears, and this loop thrashes forever
    # (the "Android is starting…" hang).  We orphaned the lock, so we clear it.
    rm -f "$WD/waydroidd.pid" 2>/dev/null
    # let the pid namespace + wayland socket tear down (matches restart.sh).
    # Nothing to wait for when nothing was running — the on-demand first
    # start, where these seconds are the user looking at a spinner.
    [ $busy = 1 ] && sleep 3
    return 0
}

# The container's /odm: the image's graft tree plus the host's AIDL graphics
# libs the A13 image lacks but the A14 Mali blobs need (common-V5 under the V4
# soname the container links against — NDK AIDL libs keep their type symbols).
# Rebuilt from the image for every generation, so nothing in it is trusted
# across starts.  Fails (and is retried) while the Halium container that
# provides /android/system is not up yet.
prepare_graft() {
    rm -rf "$WD/graft" 2>/dev/null
    mkdir -p "$WD/graft/lib" "$WD/graft/lib64" || return 1
    cp -r "$ETC/graft/." "$WD/graft/" || return 1
    chmod 755 "$WD/graft/start-allocator.sh" "$WD/graft/waydroid-net.sh"
    for a in lib64 lib; do
        cp "/android/system/$a/android.hardware.graphics.common-V5-ndk.so" \
           "$WD/graft/$a/android.hardware.graphics.common-V4-ndk.so" || return 1
        cp "/android/system/$a/android.hardware.graphics.allocator-V2-ndk.so" \
           "$WD/graft/$a/" || return 1
    done
    return 0
}

autostart() {
    case "$(param get persist.waydroid.autostart 2>/dev/null)" in
        1*) return 0 ;;
        *) return 1 ;;
    esac
}

# Seconds hidden after which an on-demand generation is given up.
idle_limit() {
    v=$(param get persist.waydroid.idle_stop_s 2>/dev/null)
    case "$v" in
        ''|*[!0-9\ ]*) echo 1800 ;;
        *) echo $((v + 0)) ;;
    esac
}

visible() {
    case "$(param get waydroid.input.grab 2>/dev/null)" in
        1*) return 0 ;;
        *) return 1 ;;
    esac
}

first_frame_seen() {
    case "$(param get waydroid.compositor.frames 2>/dev/null)" in
        1*) return 0 ;;
        *) return 1 ;;
    esac
}

# A container frozen by HIDE produces no frames; that is not a wedge.
container_frozen() {
    p=0; f=0
    while read -r k v; do
        case "$k" in
            populated) p=$v ;;
            frozen) f=$v ;;
        esac
    done </sys/fs/cgroup/waydroid/cgroup.events 2>/dev/null
    [ "$p" = 1 ] && [ "$f" = 1 ]
}

# We are the gate (see the header): tear down whatever runs and have init
# stop us.  A service init stopped itself is not restarted; one that merely
# exits is, so exiting alone would not do.
stand_down() {
    log "$1"
    [ "$2" = teardown ] && teardown
    param set ohos.ctl.stop waydroid_supervisor 2>/dev/null
    sleep 10
    exit 0
}

# Start a fresh compositor, wait for its socket, then start the container.
# Returns 0 if the container was launched, 1 if the compositor never came up.
start_generation() {
    pick_binaries
    log "starting compositor: $COMPOSITOR"
    LD_LIBRARY_PATH="$WD/bin" nohup "$COMPOSITOR" >"$WD/compositor.log" 2>&1 &
    cpid=$!
    i=0
    while [ $i -lt 150 ]; do
        [ -e "$SOCK" ] && break
        kill -0 "$cpid" 2>/dev/null || { log "compositor died during startup"; return 1; }
        sleep 0.2
        i=$((i + 1))
    done
    if [ ! -e "$SOCK" ]; then
        log "compositor socket never appeared after ${i} ticks; aborting generation"
        return 1
    fi
    log "compositor up (socket after ${i} ticks); starting container: $WAYDROIDD"
    cd "$WD" && nohup "$WAYDROIDD" >"$WD/waydroidd.log" 2>&1 &
    return 0
}

# --- main --------------------------------------------------------------
# Persist params are loaded during post-fs-data, the first stage that starts
# us; give them a moment before concluding that waydroid is off.  (Not on a
# demand start: params are long loaded by the time an app runs, and with the
# front-end beating every 2 s a disabled device would otherwise always have
# one of us sitting in this loop.)  Until we know it is on, leave no trace: no
# directory, no log, no param.
n=0
while [ -z "$DEMAND" ] && ! enabled && [ $n -lt 5 ]; do
    sleep 2
    n=$((n + 1))
done
enabled || stand_down "persist.waydroid.enabled is not 1; standing down"
if [ -z "$DEMAND" ] && ! autostart; then
    stand_down "on-demand mode: waiting for Android Apps to be opened"
fi

mkdir -p "$WD" 2>/dev/null
: >"$LOG" 2>/dev/null
if [ -n "$DEMAND" ]; then log "started on demand ($DEMAND)"; else log "started at boot (autostart)"; fi
IDLE_LIMIT=$(idle_limit)

waiting=0
while true; do
    enabled || stand_down "disabled; tearing down and standing down" teardown
    if [ ! -f "$WD/images/system.img" ] || [ ! -f "$WD/images/vendor.img" ]; then
        # Enabled but not provisioned (a fresh flash): wait for the images
        # instead of crash-looping under init.
        [ $waiting = 0 ] && log "waiting for $WD/images/{system,vendor}.img"
        waiting=1
        sleep 15
        continue
    fi
    waiting=0

    teardown
    if ! prepare_graft; then
        log "graft not ready (is /android/system mounted?); retrying"
        sleep 5
        continue
    fi
    if ! start_generation; then
        sleep 5
        continue
    fi

    # Wait for waydroidd to publish the container init pid, then block until
    # that pid exits.  A frozen container (app backgrounded) stays alive, so
    # kill -0 keeps succeeding — we only loop on a real exit.
    j=0
    while [ ! -s "$WD/container.pid" ] && [ $j -lt 150 ]; do
        sleep 0.2
        j=$((j + 1))
    done
    CPID=$(cat "$WD/container.pid" 2>/dev/null)
    if [ -z "$CPID" ]; then
        log "container.pid never appeared; rebuilding generation"
        continue
    fi
    log "container init pid=$CPID up; monitoring"

    # No reveal to do here: the front-end republishes SHOW every 2 s while it
    # is in the foreground, so a generation that comes up hidden — first
    # start, or a recovery while the user is looking at "Android is
    # starting…" — is revealed by the next beat, and one that comes up while
    # nobody is looking stays hidden.

    # Monitor.  Three ways out besides the container exiting:
    #   disabled     -> stand down (top of the outer loop)
    #   wedged       -> no first frame within 90 unfrozen seconds: rebuild.
    #                   Rare, silent and otherwise permanent (the composer
    #                   stuck in its Wayland handshake, SurfaceFlinger waiting
    #                   on IComposer, no tombstone, init alive).
    #   idle         -> on-demand only: hidden for IDLE_LIMIT seconds.
    booting=0; idle=0; framed=0
    while kill -0 "$CPID" 2>/dev/null; do
        sleep 2
        enabled || break
        if [ $framed = 0 ]; then
            if first_frame_seen; then
                framed=1
                log "first frame after ~${booting}s"
            elif ! container_frozen; then
                booting=$((booting + 2))
                if [ $booting -ge 90 ]; then
                    log "no frame from the container after ${booting}s; rebuilding generation"
                    break
                fi
            fi
        fi
        if visible; then
            idle=0
        else
            idle=$((idle + 2))
            if ! autostart && [ "$IDLE_LIMIT" -gt 0 ] && [ $idle -ge "$IDLE_LIMIT" ]; then
                stand_down "hidden for ${idle}s; on-demand generation given up" teardown
            fi
        fi
    done
    kill -0 "$CPID" 2>/dev/null || log "container init $CPID exited; recovering with a fresh generation"
done
