/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_runtime.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <acdb.h>
#include <ar_osal_error.h>
#include <json/json.h>
#include <sound/asound.h>
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
};

struct mixer_ctl {
    bool parameter = false;
    int enabled = 0;
    bool reject_enable = false;
    bool reject_disable = false;
    bool reject_parameters = false;
    Bytes bytes;
    std::vector<std::string> enums{};
    std::vector<long> integers{};
};

struct mixer {
    std::map<std::string, mixer_ctl> controls;
};

static std::set<struct pcm *> open_pcms;
static bool initialization_seen;
static bool prepared;
static int rejected_pcm = -1;
static int rejected_start = -1;
static int rejected_stop = -1;
static int rejected_open = -1;
static pcm bad_pcm{UINT32_MAX};
static int control_fd = -1;
static mixer *backend_mixer;
static unsigned backend_rate;
static std::string backend_data_format;

extern "C" int pcm_start(struct pcm *handle);

extern "C" int __real_open(const char *, int, ...);

extern "C" int __wrap_open(const char *path, int flags, ...)
{
    assert(!std::strcmp(path, "/dev/snd/controlC0"));
    control_fd = __real_open("/dev/null", flags);
    assert(control_fd >= 0);
    return control_fd;
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
    assert(fd == control_fd);
    va_list args;
    va_start(args, command);
    void *data = va_arg(args, void *);
    va_end(args);

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
    } else {
        assert(command == SNDRV_CTL_IOCTL_TLV_READ);
        uint32_t event[] = {0x4000 + tlv.numid, 0x08001000, 0};
        tlv.tlv[0] = APM_EVENT_MODULE_TO_CLIENT;
        tlv.tlv[1] = sizeof(event);
        std::memcpy(tlv.tlv + 2, event, sizeof(event));
    }

    return 0;
}

extern "C" int32_t acdb_ioctl(uint32_t command, const void *input,
                              uint32_t, void *output, uint32_t)
{
    if (command != ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_NONPERSIST)
        return AR_ENOTEXIST;

    auto *request = static_cast<const AcdbSgIdCalKeyVector *>(input);
    assert(request->num_sg_ids == 1);
    assert(!request->cal_key_vector_prior.num_keys);
    assert(!request->cal_key_vector_new.num_keys);
    auto *response = static_cast<AcdbBlob *>(output);
    uint32_t record[] = {0x4001, 0x08001026, 4, 0, request->sg_ids[0], 0};

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

    auto *handle = new pcm{device};
    handle->flags = flags;
    handle->buffer_frames = config->period_count * config->period_size;
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

    assert(unsigned(request) == SNDRV_PCM_IOCTL_STATUS);
    static_cast<snd_pcm_status *>(data)->state = handle->state;
    return 0;
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

static ar_topology::Pcm configuration(unsigned id)
{
    pcm_config config{};
    config.channels = 2;
    config.rate = 48000;
    config.period_size = 240;
    config.period_count = 4;
    config.format = PCM_FORMAT_S16_LE;
    return {id, unsigned(id == 4 ? PCM_IN : PCM_OUT), config};
}

struct Initialization {
    mixer &controls;
    std::vector<unsigned> defaults;
    bool reject = false;
};

static int initialize(void *opaque,
                      const std::vector<ar_topology::Stream> &streams) noexcept
{
    auto &value = *static_cast<Initialization *>(opaque);

    for (const auto &stream : streams)
        assert(stream.handle->state == SNDRV_PCM_STATE_SETUP);

    for (unsigned id : value.defaults) {
        const auto &data = value.controls.controls.at("set" +
                                                       std::to_string(id));
        uint32_t subgraph;
        assert(data.bytes.size() == 32);
        std::memcpy(&subgraph, data.bytes.data() + 24, sizeof(subgraph));
        assert(subgraph == 0xb0000000u + id);
    }

    initialization_seen = true;
    return value.reject ? -ECANCELED : 0;
}

static void media_configuration()
{
    using namespace ar_topology;
    auto input = fixture();
    auto &gate = input["hardware_gates"][0];
    gate["module"] = 0x4003;
    gate["dai"] = 16;
    gate["direction"] = "playback";
    auto &backend = gate["backend"];
    backend["dai"] = 16;
    backend["provider"] = "/dais";
    backend["path"] = "/sound/speaker";
    backend["name"] = "Speaker Playback";
    for (const char *key : {"rate", "format", "channels", "data_format"})
        backend["controls"][key] = std::string("backend ") + key;

    Bindings bindings(input);
    auto controls = make_mixer();
    controls.controls["backend rate"].enums = {"96000", "48000"};
    controls.controls["backend channels"].enums = {"1", "2"};
    controls.controls["backend format"].enums = {"S32_LE", "S16_LE"};
    controls.controls["backend data_format"].enums = {
        "Topology", "Compressed over PCM", "Fixed Point"};
    Runtime runtime(0, &controls, bindings);
    backend_mixer = &controls;
    backend_rate = 48000;
    backend_data_format = "Fixed Point";
    Media config{48000, 2, 1, "S16_LE"};
    HardwareConfig first{{{10, 1}}, config}, second{{{10, 2}}, config};
    SessionId a = 0, b = 0;
    std::vector<Stream> x, y;

    first.media.rate = 12345;
    assert(runtime.acquire({first.selection}, {configuration(1)}, {first}, a, x)
           == -EINVAL);
    assert(!a && x.empty() && open_pcms.empty());
    for (const char *key : {"rate", "format", "channels", "data_format"})
        assert(!controls.controls.at(std::string("backend ") + key).enabled);

    first.media = config;
    assert(!runtime.acquire({first.selection}, {configuration(1)}, {first},
                             a, x));
    assert(controls.controls.at("backend rate").enabled == 1);
    assert(controls.controls.at("backend format").enabled == 1);
    assert(controls.controls.at("backend channels").enabled == 1);
    assert(controls.controls.at("backend data_format").enabled == 2);
    assert(runtime.acquire({second.selection}, {configuration(2)}, b, y) ==
           -EBUSY);
    second.media.rate = 96000;
    assert(runtime.acquire({second.selection}, {configuration(2)}, {second},
        b, y)
           == -EBUSY);
    assert(!b && y.empty());
    assert(!controls.controls.at("path2").enabled);
    assert(!controls.controls.at("link2").enabled);
    assert(controls.controls.at("backend rate").enabled == 1);
    second.media = config;
    assert(!runtime.acquire({second.selection}, {configuration(2)}, {second},
                             b, y));
    assert(!runtime.close(a));
    a = 0;
    x.clear();
    first.media.rate = 96000;
    assert(runtime.acquire({first.selection}, {configuration(1)}, {first}, a, x)
           == -EBUSY);
    assert(!runtime.close(b));
    first.media.data_format = 7;
    backend_rate = 96000;
    backend_data_format = "Compressed over PCM";
    assert(!runtime.acquire({first.selection}, {configuration(1)}, {first},
                             a, x));
    assert(!controls.controls.at("backend rate").enabled);
    assert(!runtime.close(a));
    assert(open_pcms.empty());
    backend_mixer = nullptr;

    Json::Value combined = input["selections"][0];
    combined["graph_keys"][0][1] = "99";
    for (const auto &graph : input["selections"][2]["graphs"])
        combined["graphs"].append(graph);
    for (const auto &graph : input["selections"][2]["tuning"])
        combined["tuning"].append(graph);
    combined["references"].append("link4");
    input["selections"].append(combined);
    auto &other = input["hardware_gates"][1];
    other["module"] = 0x4005;
    other["dai"] = 120;
    other["direction"] = "capture";
    other["backend"] = backend;
    other["backend"]["dai"] = 120;
    other["backend"]["path"] = "/sound/other";
    for (const char *key : {"rate", "format", "channels", "data_format"})
        other["backend"]["controls"][key] = std::string("other ") + key;
    Bindings duplex(input);
    Runtime ambiguous(0, &controls, duplex);
    a = 0;
    x.clear();
    HardwareConfig unspecified{{{10, 99}}, config};
    assert(ambiguous.acquire({unspecified.selection},
        {configuration(1), configuration(4)}, {unspecified}, a, x) ==
        -EOPNOTSUPP);
    assert(!a && x.empty() && open_pcms.empty());

    backend["controls"].removeMember("channels");
    Bindings without_channels(input);
    Runtime missing_channels(0, &controls, without_channels);
    a = 0;
    x.clear();
    assert(missing_channels.acquire({first.selection}, {configuration(1)},
        {first}, a, x) == -EOPNOTSUPP);

    gate["dai"] = backend["dai"] = 14;
    backend["controls"]["channel_map"] = "backend channel_map";
    auto &map = controls.controls["backend channel_map"].integers;
    map = {1, 157, 0};
    Bindings codec(input);
    Runtime mapped(0, &controls, codec);
    HardwareConfig mono{{{10, 1}}, {48000, 1, 1, "S16_LE"}, {"/dais", 16}};
    assert(mapped.acquire({mono.selection}, {configuration(1)}, {mono}, a, x) ==
           -EINVAL);
    mono.backend.second = 14;
    mono.media.channels = 2;
    assert(mapped.acquire({mono.selection}, {configuration(1)}, {mono}, a, x) ==
           -EINVAL);
    mono.media.channels = 1;
    assert(!mapped.acquire({mono.selection}, {configuration(1)}, {mono}, a, x));
    Initialization tuning{controls, {1, 3}};
    map[1] = 158;
    assert(mapped.prepare(a, initialize, &tuning) == -ESTALE);
    assert(x[0].handle->state == SNDRV_PCM_STATE_SETUP);
    map[1] = 157;
    assert(!mapped.prepare(a, initialize, &tuning));
    map[1] = 158;
    assert(mapped.start(a) == -ESTALE);
    assert(x[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    map[1] = 157;
    assert(!mapped.start(a));
    map[1] = 158;
    mono.selection = {{10, 2}};
    b = 0;
    y.clear();
    assert(mapped.acquire({mono.selection}, {configuration(2)}, {mono}, b, y) ==
           -ESTALE);
    assert(!b && y.empty());
    assert(!mapped.close(a));
    assert(!mapped.acquire({mono.selection}, {configuration(2)}, {mono}, b, y));
    assert(!mapped.close(b));
    assert(open_pcms.empty());
    a = 0;
    x.clear();

    gate.removeMember("backend");
    Bindings unqualified(input);
    Runtime missing_binding(0, &controls, unqualified);
    assert(missing_binding.acquire({first.selection}, {configuration(1)},
        {first}, a, x) == -EOPNOTSUPP);
    assert(!a && x.empty() && open_pcms.empty());
}

int main()
{
    media_configuration();
    ar_topology::Bindings bindings(fixture());
    auto controls = make_mixer();
    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId first = 0, second = 0, capture = 0;
    std::vector<ar_topology::Stream> a, b, c;
    Initialization init{controls, {1, 3}};
    assert(!runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                         initialize, &init));
    assert(a[0].handle->state == SNDRV_PCM_STATE_PREPARED);

    Bytes custom{0x10, 0x20};
    auto &device = controls.controls.at("set3");
    assert(!mixer_ctl_set_array(&device, custom.data(), custom.size()));
    init.defaults = {2};
    assert(!runtime.open({{{10, 2}}}, {configuration(2)}, second, b,
                         initialize, &init));
    assert(device.bytes == custom);
    init.defaults = {4, 5};
    assert(!runtime.open({{{10, 4}}}, {configuration(4)}, capture, c,
                         initialize, &init));
    assert(!runtime.close(first));
    assert(controls.controls.at("hardware3").enabled);
    assert(!controls.controls.at("path1").enabled);
    assert(controls.controls.at("path2").enabled);
    assert(b[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    assert(c[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    assert(device.bytes == custom);
    assert(!runtime.close(second));
    assert(!controls.controls.at("hardware3").enabled);
    assert(controls.controls.at("hardware5").enabled);
    assert(!runtime.close(capture));
    assert(open_pcms.empty());

    first = 0;
    a.clear();
    init.defaults = {1, 3};
    init.reject = true;
    prepared = false;
    assert(runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                        initialize, &init) == -ECANCELED);
    assert(!first && a.empty() && open_pcms.empty() && !prepared);
    assert(!controls.controls.at("hardware3").enabled);

    init.reject = false;
    controls.controls.at("hardware3").reject_enable = true;
    controls.controls.at("hardware3").reject_disable = true;
    assert(runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                        initialize, &init) == -EIO);
    assert(!first && a.empty() && open_pcms.empty());
    controls.controls.at("hardware3").reject_enable = false;
    controls.controls.at("hardware3").reject_disable = false;
    assert(!controls.controls.at("path1").enabled);

    first = 0;
    initialization_seen = false;
    assert(!runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                         initialize, &init));
    second = 0;
    b.clear();
    assert(runtime.open({{{10, 1}}}, {configuration(1)}, second, b) == -EBUSY);
    assert(runtime.open({{{10, 6}}}, {configuration(2)}, second, b) ==
           -EOPNOTSUPP);
    assert(!controls.controls.at("path6").enabled);
    assert(a[0].handle->state == SNDRV_PCM_STATE_PREPARED);

    rejected_open = 2;
    assert(runtime.acquire({{{10, 2}}}, {configuration(2)}, second, b) == -EIO);
    assert(!second && b.empty());
    assert(controls.controls.at("hardware3").enabled);
    rejected_open = -1;
    controls.controls.at("set2").reject_parameters = true;
    assert(runtime.open({{{10, 2}}}, {configuration(2)}, second, b,
                        initialize, &init) == -EIO);
    assert(!second && device.bytes.size() == 32);
    assert(controls.controls.at("hardware3").enabled);
    assert(!controls.controls.at("path2").enabled);
    controls.controls.at("set2").reject_parameters = false;
    init.defaults = {2};
    rejected_pcm = 2;
    assert(runtime.open({{{10, 2}}}, {configuration(2)}, second, b,
                        initialize, &init) == -EPIPE);
    assert(!second && b.empty());
    assert(a[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    assert(controls.controls.at("hardware3").enabled);
    rejected_pcm = -1;
    assert(!runtime.open({{{10, 2}}}, {configuration(2)}, second, b,
                         initialize, &init));
    controls.controls.at("path1").reject_disable = true;
    assert(runtime.close(first) == -EIO);
    assert(open_pcms.size() == 1);
    assert(b[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    assert(controls.controls.at("hardware3").enabled);
    controls.controls.at("path1").reject_disable = false;
    assert(runtime.close(first) == -ENOENT);
    assert(!runtime.close(second));
    assert(open_pcms.empty());

    a.clear();
    first = 0;
    init.defaults = {1, 3, 4, 5};
    initialization_seen = false;
    assert(!runtime.open({{{10, 1}}, {{10, 4}}},
                         {configuration(1), configuration(4)}, first, a,
                         initialize, &init));
    assert(a.size() == 2);
    rejected_start = 4;
    assert(runtime.start(first) == -EIO);
    assert(a[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(a[1].handle->state == SNDRV_PCM_STATE_PREPARED);
    rejected_start = -1;
    assert(!runtime.start(first));
    assert(a[1].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(!runtime.stop(first));
    assert(!runtime.close(first));
    assert(open_pcms.empty());

    first = second = 0;
    a.clear();
    b.clear();
    prepared = false;
    assert(!runtime.acquire({{{10, 1}}}, {configuration(1)}, first, a));
    assert(!runtime.acquire({{{10, 2}}}, {configuration(2)}, second, b));
    assert(!prepared);
    assert(a[0].handle->state == SNDRV_PCM_STATE_SETUP);
    assert(b[0].handle->state == SNDRV_PCM_STATE_SETUP);
    int ring = -1;
    uint32_t ring_bytes = 0;
    std::vector<uint8_t> event{42};
    uint32_t registration[] = {0x4002, 0x08001000, 1, 0, 0, 0};
    assert(runtime.export_buffer(first, 2, ring, ring_bytes) == -ENOENT);
    assert(runtime.register_events(first, 2, registration,
        sizeof(registration)) == -ENOENT);
    assert(runtime.register_events(first, 1, registration,
        sizeof(registration)) == -ENOENT);
    assert(runtime.read_event(first, 2, event) == -ENOENT);
    int notifications = -1;
    assert(runtime.subscribe_events(first, 2, notifications) == -ENOENT);
    assert(notifications == -1);
    assert(!runtime.subscribe_events(first, 1, notifications));
    assert(fcntl(notifications, F_GETFD) & FD_CLOEXEC);
    close(notifications);
    notifications = -1;
    assert(ring == -1 && !ring_bytes && event == std::vector<uint8_t>{42});
    snd_pcm_sync_ptr sync{};
    sync.flags = SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL |
                 SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    int pcm_fd = -1;
    assert(runtime.sync_pcm(first, 2, sync) == -ENOENT);
    assert(runtime.pcm_poll_fd(first, 2, pcm_fd) == -ENOENT);
    assert(pcm_fd == -1);
    a[0].handle->position.s.status.hw_ptr = 127;
    a[0].handle->position.s.status.tstamp.tv_sec = 11;
    a[0].handle->position.s.status.tstamp.tv_nsec = 12000;
    b[0].handle->position.s.status.hw_ptr = 251;
    assert(!runtime.sync_pcm(first, 1, sync));
    assert(sync.s.status.state == SNDRV_PCM_STATE_SETUP);
    assert(sync.s.status.hw_ptr == 127);
    assert(sync.s.status.tstamp.tv_sec == 11 &&
           sync.s.status.tstamp.tv_nsec == 12000);
    assert(!runtime.sync_pcm(second, 2, sync));
    assert(sync.s.status.hw_ptr == 251);
    assert(!runtime.pcm_poll_fd(first, 1, pcm_fd));
    pollfd ready{pcm_fd, POLLOUT, 0};
    assert(poll(&ready, 1, 0) == 1 && (ready.revents & POLLOUT));
    sync.flags = SNDRV_PCM_SYNC_PTR_APPL;
    sync.c.control.avail_min = 48;
    assert(!runtime.sync_pcm(first, 1, sync));
    sync.flags = SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    sync.c.control.appl_ptr = 96;
    assert(!runtime.sync_pcm(first, 1, sync));
    sync.flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    sync.c.control.appl_ptr = sync.c.control.avail_min = 0;
    assert(!runtime.sync_pcm(first, 1, sync));
    assert(sync.c.control.appl_ptr == 96 && sync.c.control.avail_min == 48);
    const auto before = sync;
    a[0].handle->sync_error = EPIPE;
    assert(runtime.sync_pcm(first, 1, sync) == -EPIPE);
    assert(!std::memcmp(&sync, &before, sizeof(sync)));
    a[0].handle->sync_error = 0;
    a[0].handle->dup_error = ENOTTY;
    assert(runtime.export_buffer(first, 1, ring, ring_bytes) == -ENOTTY);
    assert(ring == -1 && !ring_bytes);
    a[0].handle->dup_error = 0;
    assert(!runtime.export_buffer(first, 1, ring, ring_bytes));
    assert(ring >= 0 && ring_bytes == 240 * 4 * 2 * 2);
    assert(runtime.export_buffer(first, 1, ring, ring_bytes) == -EINVAL);
    close(ring);
    ring = -1;
    registration[0] = 0x4001;
    assert(!runtime.register_events(first, 1, registration,
        sizeof(registration)));
    assert(!runtime.read_event(first, 1, event));
    uint32_t module = 0;
    assert(event.size() == 3 * sizeof(uint32_t));
    std::memcpy(&module, event.data(), sizeof(module));
    assert(module == 0x4001);
    assert(runtime.start(first) == -EBADFD);
    init.defaults = {1, 3};
    assert(!runtime.prepare(first, initialize, &init));
    assert(!mixer_ctl_set_array(&device, custom.data(), custom.size()));
    init.defaults = {2};
    assert(!runtime.prepare(second, initialize, &init));
    assert(device.bytes == custom);
    assert(!runtime.start(first));
    assert(!runtime.start(second));
    assert(!runtime.sync_pcm(first, 1, sync));
    assert(sync.s.status.hw_ptr == 127);
    assert(sync.s.status.state == SNDRV_PCM_STATE_RUNNING);
    rejected_stop = 1;
    assert(runtime.stop(first) == -EIO);
    assert(a[0].handle->state == SNDRV_PCM_STATE_SETUP);
    assert(b[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    rejected_stop = -1;
    assert(!runtime.stop(first));
    assert(a[0].handle->state == SNDRV_PCM_STATE_SETUP);
    assert(b[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(controls.controls.at("hardware3").enabled);
    assert(!runtime.prepare(first));
    assert(!runtime.start(first));
    assert(device.bytes == custom);
    assert(!runtime.stop(first));
    assert(!runtime.close(first));
    assert(b[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(!runtime.stop(second));
    assert(!runtime.close(second));
    assert(open_pcms.empty());

    first = 0;
    a.clear();
    init.defaults = {1, 3};
    assert(!runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                         initialize, &init));
    assert(!runtime.start(first));
    rejected_stop = 1;
    assert(runtime.close(first) == -EIO);
    rejected_stop = -1;
    assert(open_pcms.empty());
    assert(!controls.controls.at("hardware3").enabled);
    assert(runtime.close(first) == -ENOENT);
    assert(runtime.export_buffer(first, 1, ring, ring_bytes) == -ENOENT);
    assert(runtime.read_event(first, 1, event) == -ENOENT);
    assert(runtime.sync_pcm(first, 1, sync) == -ENOENT);
    int rejected_fd = -1;
    assert(runtime.pcm_poll_fd(first, 1, rejected_fd) == -ENOENT);
}
