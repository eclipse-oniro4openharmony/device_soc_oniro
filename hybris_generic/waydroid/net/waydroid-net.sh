#!/system/bin/sh
# W6 real networking (android_app_compat_plan.md §W6), run by
# waydroid-net.rc at sys.boot_completed.
#
# The primary path needs NO help from this script: waydroidd runs a DHCP
# server on the host end of the veth (wdb0), so the image's ethernet stack
# (networkstack IpClient) provisions eth0 with a default route + DNS that
# netd installs into the per-network policy tables app traffic uses, and
# the host MASQUERADEs 192.168.240.0/24 out the active uplink.
#
# This script is only a FALLBACK: if DHCP produced no default route a few
# seconds after boot, re-apply the W4 static-lite config so at least the
# adb rig (tcprelay -> 192.168.240.2:5555) keeps working without internet.
# The subnet route must land in netd's local_network table — Android policy
# routing never consults the main table (ip rule 32000 is `from all
# unreachable`), which blackholes data packets while TCP handshakes still
# succeed on the incoming skb's cached dst.
#
# Deployed to the graft: /odm/etc/init/waydroid-net.rc + /odm/waydroid-net.sh.

# Give DHCP a moment to land a default route (any table).
i=0
while [ "$i" -lt 8 ]; do
    if /system/bin/ip route show table all 2>/dev/null | grep -q '^default via '; then
        exit 0   # DHCP succeeded — netd owns the config, nothing to do.
    fi
    sleep 1
    i=$((i + 1))
done

# DHCP fallback (W4 static-lite): reachable adbd, no internet.
/system/bin/ip addr add 192.168.240.2/24 dev eth0 2>/dev/null
/system/bin/ip link set eth0 up
/system/bin/ip route add 192.168.240.0/24 dev eth0 table local_network 2>/dev/null
exit 0
