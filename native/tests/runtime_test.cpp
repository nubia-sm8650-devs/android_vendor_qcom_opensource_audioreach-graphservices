/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_runtime.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <sys/ioctl.h>
#include <acdb.h>
#include <ar_osal_error.h>
#include <json/json.h>
#include <sound/asound.h>
#include <sound/snd_ar_tokens.h>

using Bytes = std::vector<uint8_t>;

struct pcm {
    unsigned id;
    snd_pcm_state_t state = SNDRV_PCM_STATE_SETUP;
};

struct mixer_ctl {
    bool parameter = false;
    int enabled = 0;
    bool reject_enable = false;
    bool reject_disable = false;
    bool reject_parameters = false;
    Bytes bytes;
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

extern "C" struct pcm *pcm_open(unsigned, unsigned device, unsigned,
                                 struct pcm_config *)
{
    if (int(device) == rejected_open)
        return &bad_pcm;

    auto *handle = new pcm{device};
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
    delete handle;
    return 0;
}

extern "C" int pcm_ioctl(struct pcm *handle, int request, ...)
{
    assert(unsigned(request) == SNDRV_PCM_IOCTL_STATUS);
    va_list args;
    va_start(args, request);
    auto *status = va_arg(args, struct snd_pcm_status *);
    status->state = handle->state;
    va_end(args);
    return 0;
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
    return control->parameter ? MIXER_CTL_TYPE_BYTE : MIXER_CTL_TYPE_BOOL;
}

extern "C" unsigned mixer_ctl_get_num_values(struct mixer_ctl *)
{
    return 1;
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

int main()
{
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
    assert(runtime.start(first) == -EBADFD);
    init.defaults = {1, 3};
    assert(!runtime.prepare(first, initialize, &init));
    assert(!mixer_ctl_set_array(&device, custom.data(), custom.size()));
    init.defaults = {2};
    assert(!runtime.prepare(second, initialize, &init));
    assert(device.bytes == custom);
    assert(!runtime.start(first));
    assert(!runtime.start(second));
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
}
