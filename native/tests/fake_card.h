/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_FAKE_CARD_H
#define AR_TOPOLOGY_FAKE_CARD_H

#include "ar_topology_runtime.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <acdb.h>
#include <ar_osal_error.h>
#include <json/json.h>
#include <sound/asound.h>
#include <sound/compress_offload.h>
#include <sound/snd_ar_tokens.h>
#include <apm_api.h>

#ifndef SNDRV_PCM_IOCTL_ANONYMOUS_DUP
#define SNDRV_PCM_ANON_DUP_BUFFER 1
#define SNDRV_PCM_IOCTL_ANONYMOUS_DUP _IOW('A', 0x05, int)
#endif

using Bytes = std::vector<uint8_t>;

struct pcm {
    unsigned id;
    snd_pcm_state_t state = SNDRV_PCM_STATE_SETUP;
    unsigned flags = 0;
    int fds[2] = {-1, -1};
    snd_pcm_sync_ptr position{};
    int sync_error = 0;
    int dup_error = 0;
    unsigned buffer_frames = 0;
    pcm_config config{};
    snd_pcm_uframes_t stop_threshold = 0;
    snd_pcm_uframes_t avail_min = 0;
    snd_pcm_uframes_t start_threshold = 0;
    snd_pcm_uframes_t moved = 0;
};

struct mixer_ctl {
    bool parameter = false;
    int enabled = 0;
    bool reject_enable = false;
    bool reject_disable = false;
    bool reject_parameters = false;
    Bytes bytes;
    Bytes reply{};
    uint32_t reply_error = 0;
    std::vector<std::string> enums{};
    std::vector<long> integers{};
};

struct mixer {
    std::map<std::string, mixer_ctl> controls;
};

static std::set<struct pcm *> open_pcms;
static const struct mixer_ctl *open_requires;
static bool initialization_seen;
static bool prepared;
static int rejected_pcm = -1;
static int rejected_start = -1;
static int rejected_stop = -1;
static int rejected_open = -1;
static pcm bad_pcm{UINT32_MAX};
static int control_fd = -1;
static int compress_fd = -1;
static int compress_peer = -1;
static uint32_t compress_copied;
static std::string compress_path;
static int compress_access;
static snd_compr_params compress_params{};
static std::vector<unsigned> compress_commands;
static mixer *compress_mixer;
static bool tuned_before_params;
static mixer *backend_mixer;
static unsigned backend_rate;
static std::string backend_data_format;

static int card_watch_end = -1;
static std::vector<std::pair<int, int>> notification_ends;
static int registered_events;
static int queued_events = -1;

extern "C" int pcm_start(struct pcm *handle);

extern "C" int __real_open(const char *, int, ...);

extern "C" int __wrap_open(const char *path, int flags, ...)
{
    if (!std::strncmp(path, "/dev/snd/comprC", 15)) {
        assert(compress_fd < 0);
        compress_access = flags & O_ACCMODE;
        compress_path = path;
        int ends[2];

        assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                           0, ends));
        compress_fd = ends[0];
        compress_peer = ends[1];
        return compress_fd;
    }

    assert(!std::strcmp(path, "/dev/snd/controlC0"));

    if ((flags & O_ACCMODE) == O_RDONLY) {
        int ends[2];

        assert(card_watch_end < 0 && !pipe2(ends, O_CLOEXEC));
        card_watch_end = ends[1];
        return ends[0];
    }

    if (flags & O_NONBLOCK) {
        int ends[2];

        assert(!pipe2(ends, O_CLOEXEC | O_NONBLOCK));
        notification_ends.push_back({ends[0], ends[1]});
        return ends[0];
    }

    control_fd = __real_open("/dev/null", flags);
    assert(control_fd >= 0);
    return control_fd;
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
    va_list args;
    va_start(args, command);
    void *data = va_arg(args, void *);
    va_end(args);

    if (fd == compress_fd) {
        compress_commands.push_back(command);

        if (command == SNDRV_COMPRESS_SET_PARAMS) {
            compress_params = *static_cast<snd_compr_params *>(data);
            tuned_before_params =
                !compress_mixer->controls.at("set2").bytes.empty();
            return 0;
        }

        if (command == SNDRV_COMPRESS_TSTAMP) {
            static_cast<snd_compr_tstamp *>(data)->copied_total =
                compress_copied;
            return 0;
        }

        if (command == SNDRV_COMPRESS_START ||
            command == SNDRV_COMPRESS_NEXT_TRACK ||
            command == SNDRV_COMPRESS_PARTIAL_DRAIN ||
            command == SNDRV_COMPRESS_DRAIN)
            return 0;

        assert(command == SNDRV_COMPRESS_STOP);
        errno = EPERM;
        return -1;
    }

    bool notification = false;

    for (const auto &ends : notification_ends)
        notification |= ends.first == fd;

    assert(fd == control_fd || notification);

    if (command == SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS) {
        assert(*static_cast<int *>(data) == 1);
        return 0;
    }

    if (command == SNDRV_CTL_IOCTL_ELEM_INFO) {
        auto &info = *static_cast<snd_ctl_elem_info *>(data);
        assert(info.id.iface == SNDRV_CTL_ELEM_IFACE_PCM);
        bool found = false;
        for (auto *handle : open_pcms)
            found |= handle->id == info.id.device;
        if (!found) {
            errno = ENOENT;
            return -1;
        }

        info.id.numid = info.id.device;
        info.type = SNDRV_CTL_ELEM_TYPE_BYTES;
        info.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE;
        info.count = 65536;
        return 0;
    }

    auto &tlv = *static_cast<snd_ctl_tlv *>(data);
    if (command == SNDRV_CTL_IOCTL_TLV_WRITE) {
        assert(tlv.tlv[0] == APM_CMD_REGISTER_MODULE_EVENTS);
        registered_events++;
    } else {
        assert(command == SNDRV_CTL_IOCTL_TLV_READ);

        if (!queued_events) {
            errno = EAGAIN;
            return -1;
        }

        if (queued_events > 0)
            queued_events--;

        uint32_t event[] = {0x4000 + tlv.numid, 0x08001000, 0};
        tlv.tlv[0] = APM_EVENT_MODULE_TO_CLIENT;
        tlv.tlv[1] = sizeof(event);
        std::memcpy(tlv.tlv + 2, event, sizeof(event));
    }

    return 0;
}

static std::map<uint32_t, std::pair<ar_topology::Keys, ar_topology::Keys>>
    calibrated;

static ar_topology::Keys calibration_keys(const AcdbGraphKeyVector &vector)
{
    ar_topology::Keys keys;

    for (uint32_t i = 0; i < vector.num_keys; i++)
        keys.emplace_back(vector.graph_key_vector[i].key,
                          vector.graph_key_vector[i].value);

    return keys;
}

static int32_t (*acdb_query)(uint32_t, const void *, void *);

extern "C" int32_t acdb_ioctl(uint32_t command, const void *input,
                              uint32_t, void *output, uint32_t)
{
    if (acdb_query && command != ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_NONPERSIST &&
        command != ACDB_CMD_GET_SUBGRAPH_GLB_PSIST_IDENTIFIERS &&
        command != ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_PERSIST)
        return acdb_query(command, input, output);

    if (command != ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_NONPERSIST)
        return AR_ENOTEXIST;

    auto *request = static_cast<const AcdbSgIdCalKeyVector *>(input);
    assert(request->num_sg_ids == 1);
    auto prior = calibration_keys(request->cal_key_vector_prior);
    auto next = calibration_keys(request->cal_key_vector_new);
    calibrated[request->sg_ids[0]] = {prior, next};
    auto *response = static_cast<AcdbBlob *>(output);
    uint32_t record[] = {0x4001, 0x08001026, 4, 0, request->sg_ids[0],
                         next.empty() ? 0 : next[0].second};

    if (response->buf) {
        assert(response->buf_size == sizeof(record));
        std::memcpy(response->buf, record, sizeof(record));
    }

    response->buf_size = sizeof(record);
    return AR_EOK;
}

extern "C" struct pcm *pcm_open(unsigned, unsigned device, unsigned flags,
                                 struct pcm_config *config)
{
    if (backend_mixer) {
        auto value = [](const char *name) -> const std::string & {
            const auto &control = backend_mixer->controls.at(name);
            return control.enums.at(control.enabled);
        };
        assert(value("backend rate") == std::to_string(backend_rate));
        assert(value("backend channels") == "2");
        assert(value("backend format") == "S16_LE");
        assert(value("backend data_format") == backend_data_format);
    }
    if (int(device) == rejected_open)
        return &bad_pcm;

    if (device == 8)
        assert(!open_requires || open_requires->enabled);

    auto *handle = new pcm{device};
    handle->flags = flags;
    handle->buffer_frames = config->period_count * config->period_size;
    handle->config = *config;
    assert(!pipe2(handle->fds, O_CLOEXEC | O_NONBLOCK));
    open_pcms.insert(handle);
    return handle;
}

extern "C" int pcm_is_ready(struct pcm *handle)
{
    return handle && handle != &bad_pcm;
}

extern "C" int pcm_close(struct pcm *handle)
{
    if (handle == &bad_pcm)
        return 0;

    assert(open_pcms.erase(handle));
    close(handle->fds[0]);
    close(handle->fds[1]);
    delete handle;
    return 0;
}

extern "C" int pcm_ioctl(struct pcm *handle, int request, ...)
{
    va_list args;
    va_start(args, request);
    void *data = va_arg(args, void *);
    va_end(args);

    if (unsigned(request) == SNDRV_PCM_IOCTL_START)
        return pcm_start(handle);
    if (unsigned(request) == SNDRV_PCM_IOCTL_ANONYMOUS_DUP) {
        assert(*static_cast<int *>(data) == SNDRV_PCM_ANON_DUP_BUFFER);
        if (handle->dup_error) {
            errno = handle->dup_error;
            return -1;
        }

        return __real_open("/dev/null", O_RDWR | O_CLOEXEC);
    }
    if (unsigned(request) == SNDRV_PCM_IOCTL_SW_PARAMS) {
        auto &software = *static_cast<snd_pcm_sw_params *>(data);
        assert(handle->state != SNDRV_PCM_STATE_RUNNING);
        software.boundary = snd_pcm_uframes_t(handle->buffer_frames) << 20;
        handle->stop_threshold = software.stop_threshold;
        handle->avail_min = software.avail_min;
        return 0;
    }

    if (unsigned(request) == SNDRV_PCM_IOCTL_SYNC_PTR) {
        if (handle->sync_error) {
            errno = handle->sync_error;
            return -1;
        }

        auto &sync = *static_cast<snd_pcm_sync_ptr *>(data);
        auto &control = handle->position.c.control;
        if (!(sync.flags & SNDRV_PCM_SYNC_PTR_APPL))
            control.appl_ptr = sync.c.control.appl_ptr;
        if (!(sync.flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN))
            control.avail_min = sync.c.control.avail_min;
        sync.c.control = control;
        sync.s.status = handle->position.s.status;
        sync.s.status.state = handle->state;
        return 0;
    }

    if (unsigned(request) == SNDRV_PCM_IOCTL_WRITEI_FRAMES ||
        unsigned(request) == SNDRV_PCM_IOCTL_READI_FRAMES) {
        auto &transfer = *static_cast<snd_xferi *>(data);
        bool capture = unsigned(request) == SNDRV_PCM_IOCTL_READI_FRAMES;

        assert(capture == bool(handle->flags & PCM_IN));
        handle->moved += transfer.frames;
        transfer.result = transfer.frames;
        return 0;
    }

    if (unsigned(request) == SNDRV_PCM_IOCTL_TTSTAMP) {
        assert(*static_cast<int *>(data) == SNDRV_PCM_TSTAMP_TYPE_MONOTONIC);
        return 0;
    }

    assert(unsigned(request) == SNDRV_PCM_IOCTL_STATUS);
    static_cast<snd_pcm_status *>(data)->state = handle->state;
    static_cast<snd_pcm_status *>(data)->avail = handle->buffer_frames;
    return 0;
}

extern "C" int pcm_set_sw_config(struct pcm *handle,
                                  const struct pcm_config *config)
{
    handle->start_threshold = config->start_threshold;
    return 0;
}

extern "C" unsigned pcm_get_buffer_size(struct pcm *handle)
{
    return handle->buffer_frames;
}

extern "C" unsigned pcm_frames_to_bytes(struct pcm *handle, unsigned frames)
{
    assert(handle->config.format == PCM_FORMAT_S16_LE);
    return frames * handle->config.channels * 2;
}

extern "C" int pcm_get_poll_fd(struct pcm *handle)
{
    return handle->fds[handle->flags & PCM_IN ? 0 : 1];
}

extern "C" int pcm_prepare(struct pcm *handle)
{
    assert(initialization_seen);
    if (int(handle->id) == rejected_pcm)
        return -EPIPE;

    handle->state = SNDRV_PCM_STATE_PREPARED;
    prepared = true;
    return 0;
}

extern "C" int pcm_start(struct pcm *handle)
{
    assert(handle->state == SNDRV_PCM_STATE_PREPARED);
    if (int(handle->id) == rejected_start)
        return -EIO;

    handle->state = SNDRV_PCM_STATE_RUNNING;
    return 0;
}

extern "C" int pcm_stop(struct pcm *handle)
{
    assert(handle != &bad_pcm);
    handle->state = SNDRV_PCM_STATE_SETUP;
    if (int(handle->id) == rejected_stop)
        return -EIO;

    return 0;
}

extern "C" struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *handle,
                                                  const char *name)
{
    auto found = handle->controls.find(name);
    return found == handle->controls.end() ? nullptr : &found->second;
}

extern "C" enum mixer_ctl_type mixer_ctl_get_type(struct mixer_ctl *control)
{
    if (!control->integers.empty())
        return MIXER_CTL_TYPE_INT;
    if (!control->enums.empty())
        return MIXER_CTL_TYPE_ENUM;
    return control->parameter ? MIXER_CTL_TYPE_BYTE : MIXER_CTL_TYPE_BOOL;
}

extern "C" unsigned mixer_ctl_get_num_enums(struct mixer_ctl *control)
{
    return control->enums.size();
}

extern "C" const char *mixer_ctl_get_enum_string(struct mixer_ctl *control,
                                                unsigned item)
{
    return control->enums.at(item).c_str();
}

extern "C" unsigned mixer_ctl_get_num_values(struct mixer_ctl *control)
{
    return control->integers.empty() ? 1 : control->integers.size();
}

extern "C" int mixer_ctl_get_array(struct mixer_ctl *control, void *array,
                                    size_t count)
{
    if (control->parameter) {
        uint32_t tlv[2], header[4];

        assert(count == control->bytes.size() &&
               count >= sizeof(tlv) + sizeof(header));
        std::memcpy(tlv, control->bytes.data(), sizeof(tlv));
        assert(tlv[0] == SND_SOC_AR_TLV_GET_PARAMS &&
               tlv[1] == count - sizeof(tlv) && !(tlv[1] % 8));
        std::memcpy(header, control->bytes.data() + sizeof(tlv),
                    sizeof(header));
        assert(header[2] <= tlv[1] - sizeof(header) && !header[3]);
        header[2] = std::min<size_t>(header[2], control->reply.size());
        header[3] = control->reply_error;
        tlv[1] = sizeof(header) + header[2];

        auto *bytes = static_cast<uint8_t *>(array);

        std::memcpy(bytes, tlv, sizeof(tlv));
        std::memcpy(bytes + sizeof(tlv), header, sizeof(header));
        std::memcpy(bytes + sizeof(tlv) + sizeof(header),
                    control->reply.data(), header[2]);

        return 0;
    }

    assert(count == control->integers.size());
    std::memcpy(array, control->integers.data(), count * sizeof(long));
    return 0;
}

extern "C" int mixer_ctl_is_access_tlv_rw(struct mixer_ctl *control)
{
    return control->parameter;
}

extern "C" int mixer_ctl_get_value(struct mixer_ctl *control, unsigned)
{
    return control->enabled;
}

extern "C" int mixer_ctl_set_value(struct mixer_ctl *control, unsigned,
                                    int value)
{
    control->enabled = value;

    if ((value && control->reject_enable) ||
        (!value && control->reject_disable))
        return -EIO;

    return 0;
}

extern "C" int mixer_ctl_set_array(struct mixer_ctl *control, const void *array,
                                    size_t count)
{
    if (control->reject_parameters)
        return -EIO;

    const auto *bytes = static_cast<const uint8_t *>(array);
    control->bytes.assign(bytes, bytes + count);
    return 0;
}

static Json::Value fixture()
{
    Json::Value input;
    input["version"] = 1;
    input["selections"] = Json::Value(Json::arrayValue);
    input["frontends"] = Json::Value(Json::arrayValue);
    input["references"] = Json::Value(Json::arrayValue);
    input["hardware_gates"] = Json::Value(Json::arrayValue);

    for (unsigned id : {1, 2, 4, 6}) {
        Json::Value selection;
        Json::Value pair(Json::arrayValue);
        pair.append("0xa");
        pair.append(std::to_string(id));
        selection["graph_keys"].append(pair);
        selection["graphs"].append(id == 6 ? 2 : id);
        selection["graphs"].append(id == 4 ? 5 : 3);

        if (id == 6)
            selection["graphs"].append(6);

        for (const auto &graph : selection["graphs"]) {
            Json::Value tuning;
            tuning["graph_id"] = graph;
            tuning["subgraph"] = 0xb0000000u + graph.asUInt();
            tuning["set_control"] = "set" + graph.asString();
            tuning["modules"].append(0x4000 + graph.asUInt());
            selection["tuning"].append(tuning);
        }

        selection["references"].append(id == 6 ? "link2" :
                                       "link" + std::to_string(id));

        if (id == 6)
            selection["references"].append("link6");

        input["selections"].append(selection);
    }

    for (unsigned id : {1, 2, 4}) {
        Json::Value frontend;
        frontend["pcm_id"] = id;
        frontend["graph_id"] = id;
        frontend["direction"] = id == 4 ? "capture" : "playback";
        frontend["mode"] = "pcm";
        input["frontends"].append(frontend);
    }

    for (unsigned id : {1, 2, 4, 6}) {
        Json::Value reference;
        reference["name"] = "link" + std::to_string(id);
        reference["control"] = false;
        reference["source_graph"] = id == 4 ? 5 : id == 6 ? 3 : id;
        reference["destination_graph"] = id == 4 ? 4 : id == 6 ? 6 : 3;
        reference["path_control"] = "path" + std::to_string(id);
        input["references"].append(reference);
    }

    for (unsigned id : {3, 5, 6}) {
        Json::Value gate;
        gate["graph_id"] = id;
        gate["control"] = "hardware" + std::to_string(id);
        input["hardware_gates"].append(gate);
    }

    return input;
}

static mixer make_mixer()
{
    mixer result;

    for (unsigned id : {1, 2, 3, 4, 5, 6})
        result.controls["set" + std::to_string(id)].parameter = true;

    for (const auto &kind : {"link", "path", "hardware"})
        for (unsigned id : {1, 2, 3, 4, 5, 6})
            result.controls[std::string(kind) + std::to_string(id)];

    return result;
}

#endif
