#!/system/bin/sh
# Waydroid auto-start + crash-recovery supervisor (W5 lifecycle).
#
# init starts us from waydroid_compositor.cfg's jobs, which trigger on the
# `boot` / `post-fs-data` boot stages (both fire after /data is mounted and
# persist params are loaded) and are gated by `condition
# persist.waydroid.enabled=1`.  So we only ever run on a device where
# waydroid is enabled — X23 (param never set) never starts us and is
# unaffected.
#
# NOTE on the trigger: neither persist.waydroid.enabled nor
# bootevent.boot.completed can be the trigger.  Both are set during boot via
# a path that fires a PARAM_WATCH, not the PARAM trigger that an
# `on param:...=1` job waits for, so such a job never fires at boot (only on
# a later runtime `param set`).  The boot *stages* are posted directly by
# init, so a `boot` / `post-fs-data` job does fire; we gate it on the persist
# param via the job condition instead.
#
# When running we bring up one container "generation" — a FRESH compositor
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
# The whole container stack (waydroidd, images, data.img, the vendored .so)
# lives under /data — a dev deployment, not baked into the image — so this
# script drives /data and parks quietly (never exits, so init keeps us alive)
# whenever the param is toggled off or the stack is not yet deployed.
#
# Only shell builtins + the bins restart.sh already relies on are used
# (cat/kill/killall/param/sleep/rm/ls/nohup/pidof/cd/mkdir) — the OHOS
# toybox shell has no awk/tr/date/grep on the init PATH.

WD=/data/waydroid
LOG="$WD/supervisor.log"
SOCK="$WD/run/xdg/wayland-0"

# Prefer a dev compositor dropped in bin/ (iteration), else the image one.
COMPOSITOR="$WD/bin/waydroid_compositor"
[ -x "$COMPOSITOR" ] || COMPOSITOR=/system/bin/waydroid_compositor

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
    CPID=$(cat "$WD/container.pid" 2>/dev/null)
    [ -n "$CPID" ] && kill -9 "$CPID" 2>/dev/null
    killall -9 waydroidd 2>/dev/null
    killall -9 waydroid_compositor 2>/dev/null
    param set waydroid.compositor.ready 0 2>/dev/null
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
    # let the pid namespace + wayland socket tear down (matches restart.sh)
    sleep 3
}

# Start a fresh compositor, wait for its socket, then start the container.
# Returns 0 if the container was launched, 1 if the compositor never came up.
start_generation() {
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
    log "compositor up (socket after ${i} ticks); starting container"
    cd "$WD" && nohup ./waydroidd >"$WD/waydroidd.log" 2>&1 &
    return 0
}

# --- main --------------------------------------------------------------
# Started (cfg job condition) when persist.waydroid.enabled=1.  Supervise
# forever: never exit, so init keeps us alive as a normal service and
# restarts us if we ever crash.
: >"$LOG" 2>/dev/null
log "started; enabled=$(param get persist.waydroid.enabled 2>/dev/null)"

while true; do
    if ! enabled; then
        # Disabled: leave any running generation alone and park.  A reboot or
        # `stop waydroid_supervisor` clears us; setting the param back to 1
        # resumes on the next tick.
        sleep 30
        continue
    fi
    if [ ! -x "$WD/waydroidd" ]; then
        # Enabled but the stack is not deployed yet (e.g. a fresh flash).
        # Wait quietly instead of crash-looping under init.
        log "waiting for $WD/waydroidd to be deployed"
        sleep 30
        continue
    fi

    teardown
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

    # Recovery reveal: the launcher publishes SHOW/HIDE only on its
    # onForeground/onBackground *transitions* — edge-triggered, not sticky.
    # If this generation is a RECOVERY (the container died while the user was
    # in "Android Apps"), the fresh compositor booted hidden and there is no
    # transition to re-reveal it, so it would sit hidden behind the launcher's
    # "Android is starting…" placeholder forever.  waydroid.input.grab is the
    # level-state the compositor's ApplyVisibility keeps in sync with
    # show/hide (1 = app foreground/visible), and it survives the -9 teardown,
    # so grab=1 here means the launcher is foreground: re-fire SHOW to
    # reveal+thaw the new container.  On a normal boot the app has never been
    # foregrounded, grab is unset, and we publish nothing (container stays
    # hidden until the user opens it — OHOS keeps the panel and touch).
    case "$(param get waydroid.input.grab 2>/dev/null)" in
        1*)
            log "launcher is foreground (grab=1); re-firing SHOW to reveal container"
            /system/bin/cem publish --event org.oniroproject.waydroid.SHOW \
                >/dev/null 2>&1
            ;;
    esac

    while kill -0 "$CPID" 2>/dev/null; do
        sleep 2
        enabled || break
    done
    if enabled; then
        log "container init $CPID exited; recovering with a fresh generation"
    else
        log "disabled while monitoring; parking"
    fi
done
