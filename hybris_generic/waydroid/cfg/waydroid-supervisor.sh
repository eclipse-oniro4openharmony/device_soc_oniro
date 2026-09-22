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
#                                  Waydroid.  Its SHOW common event makes
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
# /data/waydroid holds data.img and runtime state.  The two upstream images
# are looked for in $WD/images (pushed from a host) and then in the front-end's
# own storage (downloaded by the app, so that uninstalling it reclaims the
# space).  Either way `waydroidd verify` checks them against images.manifest —
# and takes them away from the app — before anything is mounted.
#
# Talking back.  The front-end can read nothing of ours, so we publish
# org.oniroproject.waydroid.STATUS.  Its data is one state word —
#   disabled | stopped | verifying | starting | running |
#   needs-images | bad-images
# — followed by `key=value` tokens describing everything the app would
# otherwise have to read for itself (see status() below).  Events are not
# sticky, so states that last are repeated.
#
# Being told what to do.  The Waydroid app's settings page writes a control
# file into its OWN storage and we, as root, apply it (see read_control):
# start/stop, the master switch, autostart, the idle timeout.  An app can not
# set a persist param itself and must not be allowed to (the .para.dac route
# bricked a device once), but it may write its own sandbox — the same trust we
# already place in that directory for the 2 GB of images it downloads there.
#
# Only shell builtins + toybox applets known to be on the init PATH are used
# (cat/kill/killall/param/sleep/rm/ls/nohup/cd/mkdir/cp/chmod) — there is no
# awk/tr/date/grep.

DEMAND="$1"        # non-empty: started by samgr for a SHOW event
WD=/data/waydroid
ETC=/system/etc/waydroid
LOG="$WD/supervisor.log"
SOCK="$WD/run/xdg/wayland-0"
APP_FILES=/data/app/el2/100/base/org.oniroproject.androidapps/haps/entry/files
APP_IMAGES=$APP_FILES/images
CONTROL=$APP_FILES/control
STATUS_EVENT=org.oniroproject.waydroid.STATUS
IMAGES=
BAD_SIGS="|"       # |sig|sig|… of image pairs waydroidd rejected
IMG_LOC=none       # none | data ($WD/images) | app (the front-end's copy)
IMG_BYTES=0
CTL_SEQ=0          # the control file, as last read
CTL_RUN=on
CTL_ENABLED=
CTL_AUTOSTART=
CTL_IDLE=
CUR_EN=0           # the params, as last looked at
CUR_AS=0
IDLE_LIMIT=1800
SYSURL=; SYSSHA=; VENURL=; VENSHA=

# Which binaries the next generation runs.  Iteration aid, debug images only:
# one dropped in $WD/bin wins.  Never on a production image — that would be
# root code from a writable partition.  Resolved per generation, so an override
# can be added or removed without restarting us.
pick_binaries() {
    COMPOSITOR=/system/bin/waydroid_compositor
    WAYDROIDD=/system/bin/waydroidd
    AUDIO=/system/bin/waydroid_audio
    case "$(param get const.debuggable 2>/dev/null)" in
        1*)
            [ -x "$WD/bin/waydroid_compositor" ] && COMPOSITOR="$WD/bin/waydroid_compositor"
            [ -x "$WD/bin/waydroidd" ] && WAYDROIDD="$WD/bin/waydroidd"
            [ -x "$WD/bin/waydroid_audio" ] && AUDIO="$WD/bin/waydroid_audio"
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
    killall -9 waydroid_audio 2>/dev/null && busy=1
    param set waydroid.compositor.ready 0 2>/dev/null
    param set waydroid.compositor.frames 0 2>/dev/null
    param set waydroid.session.visible 0 2>/dev/null
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

# The container's /odm: the image's graft tree — the init service and VINTF
# fragment that run the host's AIDL Mali allocator inside the container, plus
# the network script.  Rebuilt from the image for every generation, so nothing
# in it is trusted across starts.  (Up to lineage-20 this also had to carry
# the host's AIDL graphics NDK libs, which the A13 image lacked; the A16 image
# ships allocator-V2-ndk and graphics.common-V4-ndk itself.)
#
# Still fails (and is retried) while the Halium container is not up: the
# allocator we are about to start lives in ITS vendor tree, which waydroidd
# binds at the container's /vendor_extra.  Copying the libs used to be what
# made us wait for it; now we say so.
prepare_graft() {
    [ -d /android/vendor/bin/hw ] || return 1
    rm -rf "$WD/graft" 2>/dev/null
    mkdir -p "$WD/graft" || return 1
    cp -r "$ETC/graft/." "$WD/graft/" || return 1
    chmod 755 "$WD/graft/start-allocator.sh" "$WD/graft/waydroid-net.sh"
    return 0
}

# --- the front-end's control file ------------------------------------------
# The Waydroid app's settings page can not set a param, so it writes what it
# wants into a file in its own storage and we apply it:
#
#   seq 7                bumped on every change; a change is applied once
#   run on|off|restart   the Start/Stop/Restart buttons; `off` means stay down
#   enabled 0|1          -> persist.waydroid.enabled    (the master switch)
#   autostart 0|1        -> persist.waydroid.autostart
#   idle_stop <seconds>  -> persist.waydroid.idle_stop_s
#
# seq is what makes a change a change: a value set by hand (hdc, a test) is
# left alone until the app asks for something NEW.  The applied seq lives in a
# runtime param, so a reboot re-applies the file once — which is only ever the
# app's own settings being restored.
read_control() {
    CTL_SEQ=0; CTL_RUN=on; CTL_ENABLED=; CTL_AUTOSTART=; CTL_IDLE=
    [ -f "$CONTROL" ] || return 0
    while read -r k v; do
        case "$k" in
            seq) case "$v" in ''|*[!0-9]*) ;; *) CTL_SEQ=$v ;; esac ;;
            run) case "$v" in on|off|restart) CTL_RUN=$v ;; esac ;;
            enabled) CTL_ENABLED=$v ;;
            autostart) CTL_AUTOSTART=$v ;;
            idle_stop) CTL_IDLE=$v ;;
        esac
    done <"$CONTROL"
    return 0
}

applied_seq() {
    v=$(param get waydroid.control.seq 2>/dev/null)
    case "$v" in
        ''|*[!0-9\ ]*) echo 0 ;;
        *) echo $((v + 0)) ;;
    esac
}

# Read the control file; if it carries a seq we have not applied, apply it.
# Returns 0 when something new was applied (the caller may have to act on
# CTL_RUN), 1 when there was nothing new.
apply_control() {
    read_control
    [ "$CTL_SEQ" = "$(applied_seq)" ] && return 1
    log "control seq $CTL_SEQ: run=$CTL_RUN enabled=$CTL_ENABLED autostart=$CTL_AUTOSTART idle_stop=$CTL_IDLE"
    case "$CTL_ENABLED" in
        0|1) param set persist.waydroid.enabled "$CTL_ENABLED" 2>/dev/null ;;
    esac
    case "$CTL_AUTOSTART" in
        0|1) param set persist.waydroid.autostart "$CTL_AUTOSTART" 2>/dev/null ;;
    esac
    case "$CTL_IDLE" in
        ''|*[!0-9]*) ;;
        *) param set persist.waydroid.idle_stop_s "$CTL_IDLE" 2>/dev/null ;;
    esac
    param set waydroid.control.seq "$CTL_SEQ" 2>/dev/null
    refresh_settings
    return 0
}

# The params as they really are, for the settings page (and for us).
refresh_settings() {
    if enabled; then CUR_EN=1; else CUR_EN=0; fi
    if autostart; then CUR_AS=1; else CUR_AS=0; fi
    IDLE_LIMIT=$(idle_limit)
}

# One state word plus everything the app can not read for itself:
#   seq=    the control seq we have applied
#   run=    on | off | restart, as we read it
#   en= as= idle=       persist.waydroid.{enabled,autostart,idle_stop_s}
#   img=    none | data | app   where the images we would use live
#   imgsz=  their size in bytes (0 when there are none)
#   sys= syssha= ven= vensha=   what to download — sent in every state, so the
#           settings page can offer a re-download without waiting to be asked
status() {
    /system/bin/cem publish -e "$STATUS_EVENT" -d "$1 seq=$CTL_SEQ run=$CTL_RUN\
 en=$CUR_EN as=$CUR_AS idle=$IDLE_LIMIT img=$IMG_LOC imgsz=$IMG_BYTES\
 sys=$SYSURL syssha=$SYSSHA ven=$VENURL vensha=$VENSHA" >/dev/null 2>&1
}

# Identity of an image pair: changes when either file is replaced.
image_sig() {
    echo "$(stat -c '%i-%s-%Y' "$1/system.img" 2>/dev/null)+$(stat -c '%i-%s-%Y' "$1/vendor.img" 2>/dev/null)"
}

# First directory holding both images that is not the pair we already know to
# be bad (so a corrupt host push does not shadow a good download, and 2 GB are
# not re-hashed every few seconds).
pick_images() {
    for d in "$WD/images" "$APP_IMAGES"; do
        [ -f "$d/system.img" ] && [ -f "$d/vendor.img" ] || continue
        case "$BAD_SIGS" in
            *"|$(image_sig "$d")|"*) continue ;;
        esac
        IMAGES="$d"
        [ "$d" = "$APP_IMAGES" ] && IMG_LOC=app || IMG_LOC=data
        sz1=$(stat -c %s "$d/system.img" 2>/dev/null)
        sz2=$(stat -c %s "$d/vendor.img" 2>/dev/null)
        IMG_BYTES=$(( ${sz1:-0} + ${sz2:-0} ))
        return 0
    done
    IMAGES=
    IMG_LOC=none
    IMG_BYTES=0
    return 1
}

# What to download, from images.manifest — read once and then carried in every
# STATUS.  $WD/mirror (debug images only) replaces the upstream location with a
# flat directory of the same zips.
read_manifest() {
    BASE=; SYSZIP=; SYSSHA=; VENZIP=; VENSHA=
    while read -r a b; do
        case "$a" in
            base-url) BASE=$b ;;
            *system*.zip) SYSZIP=$a; SYSSHA=$b ;;
            *vendor*.zip) VENZIP=$a; VENSHA=$b ;;
        esac
    done <"$ETC/images.manifest"
    SYSURL="$BASE/$SYSZIP"
    VENURL="$BASE/$VENZIP"
    case "$(param get const.debuggable 2>/dev/null)" in
        1*)
            if [ -s "$WD/mirror" ]; then
                read -r MIRROR <"$WD/mirror"
                SYSURL="$MIRROR/$SYSZIP"
                VENURL="$MIRROR/$VENZIP"
            fi
            ;;
    esac
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

# "Somebody is looking at Android": the compositor's visibility lease.  (This
# used to read waydroid.input.grab, which only the debug overlay sets now —
# the idle stop would have taken the container away from under a window.)
visible() {
    case "$(param get waydroid.session.visible 2>/dev/null)" in
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
    # The PulseAudio server the container's audio HAL connects to.  It
    # must exist before the container's audio HAL starts, and its socket
    # lives in the same /run/xdg directory waydroidd binds into the
    # container, so it has to be up before waydroidd builds the rootfs.
    # Not fatal if it fails: the container boots fine, just mute.
    log "starting audio server: $AUDIO"
    nohup "$AUDIO" >"$WD/audio.log" 2>&1 &
    apid=$!
    j=0
    while [ $j -lt 50 ]; do
        [ -e "$WD/run/xdg/pulse/native" ] && break
        kill -0 "$apid" 2>/dev/null || { log "audio server died during startup"; break; }
        sleep 0.1
        j=$((j + 1))
    done
    [ -e "$WD/run/xdg/pulse/native" ] || log "audio socket never appeared; container will be mute"

    log "compositor up (socket after ${i} ticks); starting container: $WAYDROIDD"
    cd "$WD" && WAYDROID_IMAGES="$IMAGES" nohup "$WAYDROIDD" >>"$WD/waydroidd.log" 2>&1 &
    return 0
}

# --- main --------------------------------------------------------------
# Persist params are loaded during post-fs-data, the first stage that starts
# us; give them a moment before concluding that waydroid is off.  (Not on a
# demand start: params are long loaded by the time an app runs, and with the
# front-end beating every 2 s a disabled device would otherwise always have
# one of us sitting in this loop.)  Until we know it is on, leave no trace: no
# directory, no log, no param.
read_manifest
# The app may have asked for something (the master switch, most of all) while
# we were not running: a demand start is how that request reaches us, so the
# control file is applied before the gate looks at the param.
apply_control
refresh_settings
n=0
while [ -z "$DEMAND" ] && ! enabled && [ $n -lt 5 ]; do
    sleep 2
    apply_control
    n=$((n + 1))
done
if ! enabled; then
    [ -n "$DEMAND" ] && status disabled
    stand_down "persist.waydroid.enabled is not 1; standing down"
fi
if [ "$CTL_RUN" = off ]; then
    [ -n "$DEMAND" ] && status stopped
    stand_down "stopped from the app; standing down"
fi
if [ -z "$DEMAND" ] && ! autostart; then
    stand_down "on-demand mode: waiting for the Waydroid app to be opened"
fi

mkdir -p "$WD" 2>/dev/null
: >"$LOG" 2>/dev/null
if [ -n "$DEMAND" ]; then log "started on demand ($DEMAND)"; else log "started at boot (autostart)"; fi
# Again: the control file is applied before the log exists (it is what decides
# whether there is to BE a log), and the line above has just truncated it.
log "control seq $CTL_SEQ: run=$CTL_RUN enabled=$CUR_EN autostart=$CUR_AS idle_stop=$IDLE_LIMIT"

waiting=0
waited=0
while true; do
    apply_control
    refresh_settings
    enabled || stand_down "disabled; tearing down and standing down" teardown
    if [ "$CTL_RUN" = off ]; then
        status stopped
        stand_down "stopped from the app; tearing down and standing down" teardown
    fi
    if ! pick_images; then
        # Enabled but not provisioned (a fresh flash): tell the front-end what
        # to fetch and wait, instead of crash-looping under init.
        [ $waiting = 0 ] && log "no images in $WD/images or the front-end's storage; waiting"
        waiting=1
        if [ "$BAD_SIGS" != "|" ]; then status bad-images; else status needs-images; fi
        sleep 3
        # On demand, nobody may be listening any more.  The front-end's beat
        # starts us again for as long as it really is open.
        waited=$((waited + 3))
        if ! autostart && [ $waited -ge 60 ]; then
            stand_down "still no images after ${waited}s; standing down until asked again"
        fi
        continue
    fi
    waiting=0
    waited=0

    pick_binaries
    status verifying
    : >"$WD/waydroidd.log" 2>/dev/null
    "$WAYDROIDD" verify "$IMAGES" >>"$WD/waydroidd.log" 2>&1
    rc=$?
    if [ $rc != 0 ]; then
        if [ $rc = 4 ]; then
            continue                    # vanished under us; look again
        fi
        BAD_SIGS="$BAD_SIGS$(image_sig "$IMAGES")|"
        log "images in $IMAGES rejected (see waydroidd.log); waiting for other ones"
        status bad-images
        sleep 3
        continue
    fi
    BAD_SIGS="|"
    status starting

    teardown
    if ! prepare_graft; then
        log "graft not ready (is the Halium container up?); retrying"
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

    # Monitor.  Four ways out besides the container exiting (the fourth, the
    # images disappearing, is in the loop):
    #   disabled     -> stand down (top of the outer loop)
    #   wedged       -> no first frame within 90 unfrozen seconds: rebuild.
    #                   Rare, silent and otherwise permanent (the composer
    #                   stuck in its Wayland handshake, SurfaceFlinger waiting
    #                   on IComposer, no tombstone, init alive).
    #   idle         -> on-demand only: hidden for IDLE_LIMIT seconds.
    booting=0; idle=0; framed=0; beat=0
    while kill -0 "$CPID" 2>/dev/null; do
        sleep 2
        # The app's settings page talks to us through the control file: stop,
        # restart, or a changed switch.  A restart is simply this generation
        # given up — the outer loop builds the next one.
        if apply_control && [ "$CTL_RUN" = restart ]; then
            log "restart asked for from the app; rebuilding the generation"
            break
        fi
        if [ "$CTL_RUN" = off ]; then
            status stopped
            stand_down "stopped from the app; tearing down and standing down" teardown
        fi
        enabled || break
        # Repeat the lasting states every 10 s: a settings page opened long
        # after the container came up has nothing else to go on.
        beat=$((beat + 1))
        if [ $((beat % 5)) = 0 ]; then
            refresh_settings
            [ $framed = 1 ] && status running
        fi
        # The images went away under a running container: the front-end was
        # uninstalled (or the user cleared its data).  The container keeps
        # running from the deleted files, and keeps their 2 GB allocated, for
        # as long as we let it.
        if [ ! -f "$IMAGES/system.img" ] || [ ! -f "$IMAGES/vendor.img" ]; then
            stand_down "images in $IMAGES are gone; tearing down" teardown
        fi
        if [ $framed = 0 ]; then
            if first_frame_seen; then
                framed=1
                log "first frame after ~${booting}s"
                status running
            elif ! container_frozen; then
                booting=$((booting + 2))
                status starting
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
