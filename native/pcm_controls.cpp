/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_pcm_controls.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>
#include <apm_api.h>

namespace ar_topology
{
namespace
{

struct Control {
    int fd = -1;
    snd_ctl_elem_info info{};

    ~Control()
    {
        if (fd >= 0)
            close(fd);
    }

    int open(unsigned card, uint32_t pcm, bool capture, unsigned access,
               bool nonblock = false)
    {
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/snd/controlC%u", card);
        fd = ::open(path, O_RDWR | O_CLOEXEC | (nonblock ? O_NONBLOCK : 0));
        if (fd < 0)
            return -errno;

        info.id.iface = SNDRV_CTL_ELEM_IFACE_PCM;
        info.id.device = pcm;
        std::snprintf(reinterpret_cast<char *>(info.id.name),
            sizeof(info.id.name), "%s Graph Events",
            capture ? "Capture" : "Playback");

        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
            return -errno;

        if (info.type != SNDRV_CTL_ELEM_TYPE_BYTES ||
            !(info.access & access))
            return -EINVAL;

        return 0;
    }

    int transfer(unsigned long command, std::vector<uint32_t> &words)
    {
        std::vector<uint32_t> packet(2 + words.size());
        packet[0] = info.id.numid;
        packet[1] = words.size() * sizeof(uint32_t);
        std::memcpy(packet.data() + 2, words.data(), packet[1]);

        if (ioctl(fd, command, packet.data()) < 0)
            return -errno;

        std::memcpy(words.data(), packet.data() + 2, packet[1]);

        return 0;
    }
};

}

int register_pcm_events(unsigned card, uint32_t pcm, bool capture,
                         const void *data, size_t size)
{
    if (!data || !size || size > std::numeric_limits<uint32_t>::max() - 16)
        return -EINVAL;

    try {
        Control control;
        int status = control.open(card, pcm, capture,
                                    SNDRV_CTL_ELEM_ACCESS_TLV_WRITE);
        if (status)
            return status;

        if (size > control.info.count)
            return -E2BIG;

        std::vector<uint32_t> words(2 + (size + 3) / 4);
        words[0] = APM_CMD_REGISTER_MODULE_EVENTS;
        words[1] = size;
        std::memcpy(words.data() + 2, data, size);

        return control.transfer(SNDRV_CTL_IOCTL_TLV_WRITE, words);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int read_pcm_event(unsigned card, uint32_t pcm, bool capture,
                    std::vector<uint8_t> &data)
{
    try {
        Control control;
        int status = control.open(card, pcm, capture,
                                    SNDRV_CTL_ELEM_ACCESS_TLV_READ);
        if (status)
            return status;

        size_t capacity = control.info.count;
        if (capacity > std::numeric_limits<uint32_t>::max() - 16)
            return -EOVERFLOW;

        std::vector<uint8_t> output(capacity);
        std::vector<uint32_t> words(2 + (capacity + 3) / 4);
        status = control.transfer(SNDRV_CTL_IOCTL_TLV_READ, words);
        if (status)
            return status;

        if (words[0] != APM_EVENT_MODULE_TO_CLIENT || words[1] > capacity)
            return -EPROTO;

        output.resize(words[1]);
        std::memcpy(output.data(), words.data() + 2, output.size());
        data = std::move(output);

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int subscribe_pcm_events(unsigned card, uint32_t pcm, bool capture, int &fd)
{
    if (fd >= 0)
        return -EINVAL;

    Control control;
    int status = control.open(card, pcm, capture,
                                 SNDRV_CTL_ELEM_ACCESS_TLV_READ, true);
    if (status)
        return status;

    int subscribe = 1;
    if (ioctl(control.fd, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe) < 0)
        return -errno;

    fd = control.fd;
    control.fd = -1;

    return 0;
}

int consume_pcm_notifications(int fd)
{
    snd_ctl_event events[16];

    for (;;) {
        ssize_t size = ::read(fd, events, sizeof(events));
        if (size > 0)
            return size % sizeof(events[0]) ? -EPROTO : 0;

        if (size < 0 && errno == EINTR)
            continue;

        if (size < 0)
            return errno == EAGAIN ? 0 : -errno;

        return -ENODEV;
    }
}

}
