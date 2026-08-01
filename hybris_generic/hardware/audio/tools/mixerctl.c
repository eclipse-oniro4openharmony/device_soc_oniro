/*
 * Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * mixerctl — read and write ALSA mixer controls.
 *
 * There is no tinymix, amixer or any other mixer CLI on this image, and the
 * audio HAL that matters here is an Android blob driven in-process through
 * libhybris whose own logs go nowhere (its liblog cannot reach the
 * container's logd from an OHOS process).  The codec's control state is
 * therefore the only externally visible record of what that HAL did, which
 * makes "diff the mixer across a state change" the primary debugging move
 * for anything audio on this port:
 *
 *     mixerctl > /data/local/tmp/before
 *     <make the call / start the stream>
 *     mixerctl > /data/local/tmp/after
 *     diff before after
 *
 * Not installed into the image — build it and push it:
 *
 *     ninja -C out/hybris_generic mixerctl
 *     hdc file send out/hybris_generic/.../mixerctl /data/local/tmp/
 *
 * Raw ioctls rather than alsa-lib, for the same reason the audio VDI pokes
 * the mixer that way: it keeps the tool to one file with no dependency
 * beyond the uapi header.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

#define CARD_CONTROL "/dev/snd/controlC0"
/* Enough for the widest control on this codec; the read is truncated to the
 * control's real count anyway. */
#define MAX_PRINT 8

static int OpenCard(void)
{
    int fd = open(CARD_CONTROL, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", CARD_CONTROL, strerror(errno));
    }
    return fd;
}

/* Print one control's value, given an id that already carries numid/name. */
static void PrintControl(int fd, struct snd_ctl_elem_id *id)
{
    struct snd_ctl_elem_info info;
    struct snd_ctl_elem_value val;
    unsigned int i, count;

    memset(&info, 0, sizeof(info));
    info.id = *id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
        printf("%s = <info: %s>\n", id->name, strerror(errno));
        return;
    }

    memset(&val, 0, sizeof(val));
    val.id = *id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
        /* Write-only controls are normal on this codec, not an error. */
        printf("%s = <read: %s>\n", id->name, strerror(errno));
        return;
    }

    count = info.count > MAX_PRINT ? MAX_PRINT : info.count;
    printf("%s =", id->name);
    for (i = 0; i < count; i++) {
        switch (info.type) {
            case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
            case SNDRV_CTL_ELEM_TYPE_INTEGER:
                printf(" %ld", (long)val.value.integer.value[i]);
                break;
            case SNDRV_CTL_ELEM_TYPE_INTEGER64:
                printf(" %lld", (long long)val.value.integer64.value[i]);
                break;
            case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
                printf(" %u", val.value.enumerated.item[i]);
                break;
            case SNDRV_CTL_ELEM_TYPE_BYTES:
                printf(" %02x", val.value.bytes.data[i]);
                break;
            default:
                printf(" ?");
                break;
        }
    }
    printf("\n");
}

static int DumpAll(int fd)
{
    struct snd_ctl_elem_list list;
    struct snd_ctl_elem_id *ids;
    unsigned int i;

    memset(&list, 0, sizeof(list));
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        fprintf(stderr, "ELEM_LIST(count): %s\n", strerror(errno));
        return 1;
    }
    if (list.count == 0) {
        return 0;
    }

    ids = calloc(list.count, sizeof(*ids));
    if (ids == NULL) {
        fprintf(stderr, "out of memory for %u controls\n", list.count);
        return 1;
    }
    list.space = list.count;
    list.pids = ids;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        fprintf(stderr, "ELEM_LIST(ids): %s\n", strerror(errno));
        free(ids);
        return 1;
    }

    for (i = 0; i < list.used; i++) {
        PrintControl(fd, &ids[i]);
    }
    free(ids);
    return 0;
}

static void FillIdByName(struct snd_ctl_elem_id *id, const char *name)
{
    memset(id, 0, sizeof(*id));
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy((char *)id->name, name, sizeof(id->name) - 1);
}

static int ReadOne(int fd, const char *name)
{
    struct snd_ctl_elem_id id;
    FillIdByName(&id, name);
    PrintControl(fd, &id);
    return 0;
}

static int WriteOne(int fd, const char *name, long value)
{
    struct snd_ctl_elem_value val;
    struct snd_ctl_elem_id id;

    FillIdByName(&id, name);
    memset(&val, 0, sizeof(val));
    val.id = id;
    /* Read first so a multi-value control keeps the members we are not
     * setting, and so a missing control fails here rather than silently. */
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
        fprintf(stderr, "read %s: %s\n", name, strerror(errno));
        return 1;
    }
    val.value.integer.value[0] = value;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) {
        fprintf(stderr, "write %s: %s\n", name, strerror(errno));
        return 1;
    }
    PrintControl(fd, &id);
    return 0;
}

int main(int argc, char **argv)
{
    int fd;
    int rc;

    fd = OpenCard();
    if (fd < 0) {
        return 1;
    }

    if (argc == 1) {
        rc = DumpAll(fd);
    } else if (argc == 2) {
        rc = ReadOne(fd, argv[1]);
    } else if (argc == 3) {
        rc = WriteOne(fd, argv[1], strtol(argv[2], NULL, 0));
    } else {
        fprintf(stderr, "usage: %s [<control> [<value>]]\n", argv[0]);
        rc = 1;
    }

    close(fd);
    return rc;
}
