/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology.h"

#include <cerrno>
#include <cstring>
#include <new>
#include <set>
#include <utility>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include <tinyalsa/asoundlib.h>
#include <sound/snd_ar_tokens.h>

namespace ar_topology
{

int prepare(struct pcm *pcm, struct mixer *mixer,
             const std::vector<Graph> &new_graphs)
{
    return prepare(pcm, mixer, new_graphs, nullptr, nullptr);
}

int prepare(struct pcm *pcm, struct mixer *mixer,
             const std::vector<Graph> &new_graphs,
             Initialize initialize, void *context)
{
    if (!pcm || !mixer)
        return -EINVAL;

    struct snd_pcm_status state{};
    int status = pcm_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &state);
    if (status)
        return status == -1 ? -errno : status;

    if (state.state != SNDRV_PCM_STATE_SETUP)
        return -EBADFD;

    status = tune(mixer, new_graphs);
    if (status)
        return status;

    if (initialize) {
        status = initialize(context);
        if (status)
            return status;
    }

    status = pcm_prepare(pcm);

    return status == -1 ? -errno : status;
}

int tune(struct mixer *mixer, const std::vector<Graph> &new_graphs)
{
    if (!mixer)
        return -EINVAL;

    struct Pending {
        struct mixer_ctl *control;
        std::vector<uint8_t> bytes;
    };

    try {
        std::vector<Pending> pending;
        std::set<uint32_t> subgraphs;
        std::set<std::string> controls;

        for (const auto &graph : new_graphs) {
            if (!subgraphs.insert(graph.subgraph).second ||
                !controls.insert(graph.set_control).second)
                return -EINVAL;

            auto *control = mixer_get_ctl_by_name(mixer,
                                                  graph.set_control.c_str());
            if (!control)
                return -ENOENT;

            if (mixer_ctl_get_type(control) != MIXER_CTL_TYPE_BYTE ||
                !mixer_ctl_is_access_tlv_rw(control))
                return -EINVAL;

            std::vector<uint8_t> data;
            int status = read_defaults(graph.subgraph, data);
            if (status)
                return status;

            if (data.empty())
                continue;

            uint32_t header[] = {SND_SOC_AR_TLV_SET_PARAMS,
                                 uint32_t(data.size())};
            Pending value{control, std::vector<uint8_t>(sizeof(header) +
                                                       data.size())};
            std::memcpy(value.bytes.data(), header, sizeof(header));
            std::memcpy(value.bytes.data() + sizeof(header), data.data(),
                         data.size());
            pending.push_back(std::move(value));
        }

        for (const auto &value : pending) {
            int status = mixer_ctl_set_array(value.control, value.bytes.data(),
                                             value.bytes.size());
            if (status)
                return status == -1 ? -errno : status;
        }

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

}
