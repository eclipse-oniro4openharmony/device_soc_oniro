#!/system/bin/sh
# W4 networking-lite (android_app_compat_plan.md §W4), run by
# waydroid-net.rc at sys.boot_completed.
#
# waydroidd assigns the static veth address before Android boots, but
# netd/EthernetTracker flushes eth0's IPv4 during boot (the image's
# ethernet stack expects to DHCP it — that lands with W6), so re-apply
# it here.  The subnet route must land in netd's local_network table:
# Android policy routing never consults the main table (ip rule 32000
# is `from all unreachable`), which blackholes data packets while TCP
# handshakes still succeed on the incoming skb's cached dst.
/system/bin/ip addr add 192.168.240.2/24 dev eth0 2>/dev/null
/system/bin/ip link set eth0 up
/system/bin/ip route add 192.168.240.0/24 dev eth0 table local_network 2>/dev/null
exit 0
