/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_pcm_controls.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sound/asound.h>
#include <apm_api.h>

using Bytes = std::vector<uint8_t>;
using Identity = std::tuple<unsigned, unsigned, std::string>;

struct Endpoint {
    unsigned numid;
    Bytes registration{};
    std::deque<Bytes> events{};
    bool lost = false;
};

static std::map<Identity, Endpoint> endpoints;
static std::set<int> controls;

extern "C" int __real_open(const char *, int, ...);
extern "C" int __real_close(int);

extern "C" int __wrap_open(const char *path, int flags, ...)
{
    assert(std::string(path) == "/dev/snd/controlC7");
    assert(flags & O_CLOEXEC);
    int original = __real_open("/dev/null", flags);
    assert(original >= 0);
    int fd = fcntl(original, F_DUPFD_CLOEXEC, 10);
    assert(fd >= 0);
    __real_close(original);
    controls.insert(fd);
    return fd;
}

extern "C" int __wrap_close(int fd)
{
    controls.erase(fd);
    return __real_close(fd);
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
    assert(controls.count(fd));
    va_list args;
    va_start(args, command);
    void *data = va_arg(args, void *);
    va_end(args);

    if (command == SNDRV_CTL_IOCTL_ELEM_INFO) {
        auto &info = *static_cast<snd_ctl_elem_info *>(data);
        assert(!info.id.numid && !info.id.index && !info.id.subdevice);
        auto found = endpoints.find({info.id.iface, info.id.device,
            reinterpret_cast<const char *>(info.id.name)});
        if (found == endpoints.end()) {
            errno = ENOENT;
            return -1;
        }

        info.id.numid = found->second.numid;
        info.type = SNDRV_CTL_ELEM_TYPE_BYTES;
        info.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE;
        info.count = 65536;
        return 0;
    }

    auto &tlv = *static_cast<snd_ctl_tlv *>(data);
    Endpoint *endpoint = nullptr;
    for (auto &[id, value] : endpoints) {
        (void)id;
        if (value.numid == tlv.numid)
            endpoint = &value;
    }
    assert(endpoint);
    auto *words = tlv.tlv;

    if (command == SNDRV_CTL_IOCTL_TLV_WRITE) {
        assert(words[0] == APM_CMD_REGISTER_MODULE_EVENTS);
        assert(words[1] <= tlv.length - 8);
        auto *begin = reinterpret_cast<uint8_t *>(words + 2);
        endpoint->registration.assign(begin, begin + words[1]);
        return 0;
    }

    assert(command == SNDRV_CTL_IOCTL_TLV_READ);
    if (endpoint->lost) {
        endpoint->lost = false;
        errno = EOVERFLOW;
        return -1;
    }
    if (endpoint->events.empty()) {
        errno = EAGAIN;
        return -1;
    }

    auto &event = endpoint->events.front();
    assert(event.size() <= tlv.length - 8);
    words[0] = APM_EVENT_MODULE_TO_CLIENT;
    words[1] = event.size();
    std::memcpy(words + 2, event.data(), event.size());
    endpoint->events.pop_front();
    return 0;
}

int main()
{
    using namespace ar_topology;
    unsigned pcm = SNDRV_CTL_ELEM_IFACE_PCM;
    unsigned mixer = SNDRV_CTL_ELEM_IFACE_MIXER;
    endpoints.emplace(Identity{mixer, 0, "Playback Graph Events"},
                        Endpoint{1});
    auto &events = endpoints.emplace(
        Identity{pcm, 9, "Playback Graph Events"}, Endpoint{5}).first->second;
    auto &other = endpoints.emplace(
        Identity{pcm, 2, "Playback Graph Events"}, Endpoint{6}).first->second;

    uint32_t record[] = {0x4001, 0x08001000, 1, 0, 4, 0, 123};
    Bytes registration(sizeof(record));
    std::memcpy(registration.data(), record, sizeof(record));
    assert(!register_pcm_events(7, 9, false,
                                 registration.data(), registration.size()));
    assert(events.registration == registration && other.registration.empty());
    events.events = {registration, Bytes{9, 8, 7}};
    events.lost = true;
    Bytes output{42};
    assert(read_pcm_event(7, 9, false, output) == -EOVERFLOW);
    assert(output == Bytes{42});
    assert(!read_pcm_event(7, 9, false, output) && output == registration);
    assert(!read_pcm_event(7, 9, false, output));
    assert((output == Bytes{9, 8, 7}));
    assert(read_pcm_event(7, 9, false, output) == -EAGAIN);
    assert((output == Bytes{9, 8, 7}));
    assert(controls.empty());
}
