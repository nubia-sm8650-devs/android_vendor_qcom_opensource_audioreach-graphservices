/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <map>
#include <set>
#include <sys/ioctl.h>
#include <acdb.h>
#include <ar_osal_error.h>
#include <sound/asound.h>
#include <sound/snd_ar_tokens.h>
#include <tinyalsa/asoundlib.h>

using Bytes = std::vector<uint8_t>;

struct pcm {
    snd_pcm_state_t state = SNDRV_PCM_STATE_SETUP;
};

struct mixer_ctl {
    Bytes received;
    bool reject = false;
};

struct mixer {
    std::map<std::string, mixer_ctl> controls;
};

static std::map<uint32_t, Bytes> defaults;
static std::set<uint32_t> persistent;
static mixer *active_mixer;
static std::map<std::string, Bytes> expected;

extern "C" int32_t acdb_ioctl(uint32_t command, const void *input,
                              uint32_t input_size, void *output,
                              uint32_t output_size)
{
    assert(input_size == sizeof(AcdbSgIdCalKeyVector));
    auto *request = static_cast<const AcdbSgIdCalKeyVector *>(input);
    assert(request->num_sg_ids == 1);
    assert(!request->cal_key_vector_prior.num_keys);
    assert(!request->cal_key_vector_new.num_keys);
    uint32_t sg = request->sg_ids[0];

    if (command == ACDB_CMD_GET_SUBGRAPH_GLB_PSIST_IDENTIFIERS)
        return AR_ENOTEXIST;
    if (command == ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_PERSIST) {
        if (!persistent.count(sg))
            return AR_ENOTEXIST;

        assert(output_size == sizeof(AcdbSgIdPersistCalData));
        auto *response = static_cast<AcdbSgIdPersistCalData *>(output);
        response->num_sg_ids = 1;
        response->cal_data_size = 32;
        return AR_EOK;
    }

    assert(command == ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_NONPERSIST);
    assert(output_size == sizeof(AcdbBlob));

    if (!defaults.count(sg))
        return AR_ENOTEXIST;

    auto *response = static_cast<AcdbBlob *>(output);
    const auto &data = defaults.at(sg);

    if (response->buf) {
        assert(response->buf_size >= data.size());
        std::memcpy(response->buf, data.data(), data.size());
    }

    response->buf_size = data.size();
    return AR_EOK;
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
    for (const auto &[name, data] : expected)
        assert(active_mixer->controls.at(name).received == data);

    handle->state = SNDRV_PCM_STATE_PREPARED;
    return 0;
}

extern "C" struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *handle,
                                                  const char *name)
{
    auto it = handle->controls.find(name);
    return it == handle->controls.end() ? nullptr : &it->second;
}

extern "C" enum mixer_ctl_type mixer_ctl_get_type(struct mixer_ctl *)
{
    return MIXER_CTL_TYPE_BYTE;
}

extern "C" int mixer_ctl_is_access_tlv_rw(struct mixer_ctl *)
{
    return 1;
}

extern "C" int mixer_ctl_set_array(struct mixer_ctl *control, const void *array,
                                    size_t count)
{
    if (control->reject) {
        errno = EIO;
        return -1;
    }

    const auto *data = static_cast<const uint8_t *>(array);
    control->received.assign(data, data + count);
    return 0;
}

static Bytes tlv(const Bytes &data)
{
    uint32_t header[] = {SND_SOC_AR_TLV_SET_PARAMS, uint32_t(data.size())};
    Bytes result(sizeof(header) + data.size());
    std::memcpy(result.data(), header, sizeof(header));
    std::memcpy(result.data() + sizeof(header), data.data(), data.size());
    return result;
}

int main()
{
    pcm handle;
    mixer controls;
    active_mixer = &controls;
    controls.controls.emplace("ar_g256_set", mixer_ctl{});
    controls.controls.emplace("ar_g257_set", mixer_ctl{});
    uint32_t parameter[] = {0x4001, 0x08001026, 4, 0, 1, 0};
    Bytes data(sizeof(parameter));
    std::memcpy(data.data(), parameter, sizeof(parameter));
    defaults.emplace(0xB0000001, data);
    expected.emplace("ar_g256_set", tlv(data));
    std::vector<ar_topology::Graph> graphs{
        {0xB0000001, "ar_g256_set"}, {0xB0000002, "ar_g257_set"}};
    assert(!ar_topology::prepare(&handle, &controls, graphs));
    assert(handle.state == SNDRV_PCM_STATE_PREPARED);
    assert(controls.controls.at("ar_g257_set").received.empty());
    assert(ar_topology::prepare(&handle, &controls, graphs) == -EBADFD);

    handle.state = SNDRV_PCM_STATE_SETUP;
    controls.controls.at("ar_g256_set").received.clear();
    persistent.insert(0xB0000002);
    assert(ar_topology::prepare(&handle, &controls, graphs) == -EOPNOTSUPP);
    assert(handle.state == SNDRV_PCM_STATE_SETUP);
    assert(controls.controls.at("ar_g256_set").received.empty());

    persistent.clear();
    controls.controls.at("ar_g256_set").reject = true;
    assert(ar_topology::prepare(&handle, &controls, graphs) == -EIO);
    assert(handle.state == SNDRV_PCM_STATE_SETUP);
    controls.controls.at("ar_g256_set").reject = false;
    auto initialize = [](void *opaque) noexcept {
        auto *handle = static_cast<pcm *>(opaque);
        assert(handle->state == SNDRV_PCM_STATE_SETUP);
        assert(active_mixer->controls.at("ar_g256_set").received ==
               expected.at("ar_g256_set"));
        return -ECANCELED;
    };
    assert(ar_topology::prepare(&handle, &controls, graphs, initialize,
                                &handle) == -ECANCELED);
    assert(handle.state == SNDRV_PCM_STATE_SETUP);
    assert(!ar_topology::prepare(&handle, &controls, graphs));
}
