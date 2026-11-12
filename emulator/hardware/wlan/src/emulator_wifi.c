/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * emulator_wifi: sets up the emulator's simulated WiFi at boot.
 *
 * With run.sh --wifi the guest has a second virtio NIC (MAC 52:54:00:12:36:xx)
 * on its own QEMU user network. This puts the kernel's virt_wifi driver on
 * top of it, as the Android emulator does: wlan0 is a cfg80211 station that
 * scans one open network, "VirtWifi", and once connected carries its traffic
 * over that NIC. The stock WiFi stack (chip HDI over nl80211, wpa_supplicant,
 * the framework DHCP client) then treats it as real WiFi.
 *
 * The first NIC, eth0, stays for hdc only. It is given a static address in
 * LAN mode through the ethernet service's user configuration, so it does not
 * compete with WiFi for the default network (ethernet always wins).
 *
 * Without the WiFi NIC (plain run.sh) this only removes the LAN configuration
 * it wrote before, so eth0 is a normal internet link again.
 *
 * It runs once from init at post-fs-data, before the network services start.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD001560
#define LOG_TAG "EmulatorWifi"
#define LOGI(...) HILOG_INFO(LOG_CORE, __VA_ARGS__)
#define LOGE(...) HILOG_ERROR(LOG_CORE, __VA_ARGS__)

#define WIFI_NIC_MAC_PREFIX "52:54:00:12:36:"
#define LOWER_NAME "vwifi0"  /* not eth<n>/usb<n> (ethernet service), not wlan* (WiFi HAL) */
#define WLAN_NAME "wlan0"
#define LAN_IFACE "eth0"
#define LAN_ADDR "10.0.3.15"
#define LAN_MASK "255.255.255.0"
#define LAN_NET "10.0.3.0"
#define ETH_CONFIG_DIR "/data/service/el1/public/netmanager/ethernet"
#define ETH_CONFIG ETH_CONFIG_DIR "/" LAN_IFACE
#define MARKER "# written by emulator_wifi"

static int FindWifiNic(char *name, size_t len)
{
    DIR *dir = opendir("/sys/class/net");
    if (dir == NULL) {
        return -1;
    }
    struct dirent *e;
    int found = -1;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[256];
        char mac[32] = {0};
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", e->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        if (fgets(mac, sizeof(mac), f) != NULL &&
            strncmp(mac, WIFI_NIC_MAC_PREFIX, strlen(WIFI_NIC_MAC_PREFIX)) == 0) {
            snprintf(name, len, "%s", e->d_name);
            found = 0;
        }
        fclose(f);
        if (found == 0) {
            break;
        }
    }
    closedir(dir);
    return found;
}

static int SetUp(int sock, const char *name, int up)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        return -1;
    }
    if (up) {
        ifr.ifr_flags |= IFF_UP;
    } else {
        ifr.ifr_flags &= ~IFF_UP;
    }
    return ioctl(sock, SIOCSIFFLAGS, &ifr);
}

static int Rename(int sock, const char *from, const char *to)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", from);
    snprintf(ifr.ifr_newname, IFNAMSIZ, "%s", to);
    return ioctl(sock, SIOCSIFNAME, &ifr);
}

static unsigned int IfIndex(const char *name)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        return 0;
    }
    int ret = ioctl(sock, SIOCGIFINDEX, &ifr);
    close(sock);
    return ret < 0 ? 0 : (unsigned int)ifr.ifr_ifindex;
}

static void AddAttr(struct nlmsghdr *n, size_t max, int type, const void *data, size_t len)
{
    size_t total = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(RTA_LENGTH(len));
    if (total > max) {
        return;
    }
    struct rtattr *rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)RTA_LENGTH(len);
    if (len > 0) {
        memcpy(RTA_DATA(rta), data, len);
    }
    n->nlmsg_len = (unsigned int)total;
}

/* ip link add link LOWER_NAME name WLAN_NAME type virt_wifi */
static int CreateVirtWifi(unsigned int lowerIndex)
{
    struct {
        struct nlmsghdr n;
        struct ifinfomsg i;
        char buf[512];
    } req;
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type = RTM_NEWLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.i.ifi_family = AF_UNSPEC;

    AddAttr(&req.n, sizeof(req), IFLA_IFNAME, WLAN_NAME, strlen(WLAN_NAME) + 1);
    AddAttr(&req.n, sizeof(req), IFLA_LINK, &lowerIndex, sizeof(lowerIndex));
    struct rtattr *linkInfo = (struct rtattr *)((char *)&req + NLMSG_ALIGN(req.n.nlmsg_len));
    AddAttr(&req.n, sizeof(req), IFLA_LINKINFO, NULL, 0);
    AddAttr(&req.n, sizeof(req), IFLA_INFO_KIND, "virt_wifi", strlen("virt_wifi"));
    linkInfo->rta_len = (unsigned short)((char *)&req + req.n.nlmsg_len - (char *)linkInfo);

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        return -errno;
    }
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (sendto(fd, &req, req.n.nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int err = -errno;
        close(fd);
        return err;
    }
    char reply[1024];
    ssize_t n = recv(fd, reply, sizeof(reply), 0);
    close(fd);
    if (n < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
        return -EIO;
    }
    struct nlmsghdr *h = (struct nlmsghdr *)reply;
    if (h->nlmsg_type == NLMSG_ERROR) {
        return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
    }
    return 0;
}

static int WriteLanConfig(void)
{
    struct passwd *pw = getpwnam("net_manager");
    uid_t uid = pw != NULL ? pw->pw_uid : 0;
    gid_t gid = pw != NULL ? pw->pw_gid : 0;
    mkdir("/data/service/el1/public/netmanager", 0711);
    mkdir(ETH_CONFIG_DIR, 0755);
    if (chown("/data/service/el1/public/netmanager", uid, gid) != 0 || chown(ETH_CONFIG_DIR, uid, gid) != 0) {
        LOGE("cannot set the owner of %{public}s: %{public}d", ETH_CONFIG_DIR, errno);
    }
    FILE *f = fopen(ETH_CONFIG, "w");
    if (f == NULL) {
        return -1;
    }
    /* The ROUTE puts the subnet into the local network's table (99), which
     * the policy rules consult before the default network's: without it the
     * replies to hdc's host side leave through wlan0 once WiFi connects. */
    fprintf(f, MARKER "\nDEVICE=" LAN_IFACE "\nBOOTPROTO=LAN_STATIC\nIPADDR=" LAN_ADDR "\nNETMASK=" LAN_MASK
        "\nROUTE=" LAN_NET "\nROUTE_NETMASK=" LAN_MASK "\n");
    fclose(f);
    if (chown(ETH_CONFIG, uid, gid) != 0 || chmod(ETH_CONFIG, 0644) != 0) {
        LOGE("cannot set the owner of %{public}s: %{public}d", ETH_CONFIG, errno);
    }
    return 0;
}

static void RemoveLanConfig(void)
{
    char head[64] = {0};
    FILE *f = fopen(ETH_CONFIG, "r");
    if (f == NULL) {
        return;
    }
    size_t n = fread(head, 1, strlen(MARKER), f);
    fclose(f);
    /* Only our own file: a configuration made in Settings stays. */
    if (n == strlen(MARKER) && memcmp(head, MARKER, n) == 0) {
        unlink(ETH_CONFIG);
        LOGI("ethernet mode: removed the LAN configuration of " LAN_IFACE);
    }
}

int main(void)
{
    char nic[IFNAMSIZ] = {0};
    if (FindWifiNic(nic, sizeof(nic)) != 0) {
        RemoveLanConfig();
        return 0;
    }
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        return 1;
    }
    if (strcmp(nic, LOWER_NAME) != 0) {
        SetUp(sock, nic, 0);
        if (Rename(sock, nic, LOWER_NAME) != 0) {
            LOGE("cannot rename %{public}s: %{public}d", nic, errno);
            close(sock);
            return 1;
        }
    }
    if (SetUp(sock, LOWER_NAME, 1) != 0) {
        LOGE("cannot bring up " LOWER_NAME ": %{public}d", errno);
    }
    close(sock);

    unsigned int index = IfIndex(LOWER_NAME);
    int ret = IfIndex(WLAN_NAME) != 0 ? 0 : CreateVirtWifi(index);
    if (ret != 0) {
        LOGE("cannot create " WLAN_NAME " on " LOWER_NAME ": %{public}d", ret);
        return 1;
    }
    if (WriteLanConfig() != 0) {
        LOGE("cannot write %{public}s: %{public}d", ETH_CONFIG, errno);
    }
    LOGI("WiFi mode: " WLAN_NAME " (virt_wifi) on %{public}s, " LAN_IFACE " is LAN only", nic);
    return 0;
}
