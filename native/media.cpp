/* SPDX-License-Identifier: BSD-3-Clause */
#include "media.h"

#include <cerrno>
#include <utility>
#include <tinyalsa/asoundlib.h>

namespace ar_topology
{

int media_writes(struct mixer *mixer, const Backend &backend,
                   const Media &media, std::vector<MediaWrite> &writes)
{
    static const char *const data_formats[] = {
        "Topology", "Fixed Point", "IEC61937", "IEC60958", "DSD over PCM",
        "Generic Compressed", "Raw Compressed", "Compressed over PCM",
        "IEC60958 Non-Linear",
    };
    if (!media.data_format || media.data_format >=
        sizeof(data_formats) / sizeof(data_formats[0]))
        return -EINVAL;

    if (!backend.controls.count("channels") &&
        !backend.controls.count("channel_map"))
        return -EOPNOTSUPP;

    std::map<std::string, std::string> values{
        {"rate", std::to_string(media.rate)},
        {"channels", std::to_string(media.channels)},
        {"format", media.format},
        {"data_format", data_formats[media.data_format]},
    };

    for (const auto &[key, value] : values) {
        if (key == "channels" && backend.controls.count("channel_map"))
            continue;

        auto *control = mixer_get_ctl_by_name(mixer,
            backend.controls.at(key).c_str());
        if (!control)
            return -ENOENT;

        if (mixer_ctl_get_type(control) != MIXER_CTL_TYPE_ENUM ||
            mixer_ctl_get_num_values(control) != 1)
            return -EINVAL;

        unsigned item = 0, count = mixer_ctl_get_num_enums(control);

        for (; item < count; item++) {
            const char *name = mixer_ctl_get_enum_string(control, item);
            if (name && value == name)
                break;
        }

        if (item == count)
            return -EINVAL;

        int current = mixer_ctl_get_value(control, 0);
        if (current < 0)
            return current == -1 ? -errno : current;

        if (unsigned(current) != item)
            writes.push_back({control, item});
    }

    return 0;
}

int media_channel_map(struct mixer *mixer, const Backend &backend,
                        const Media &media, std::vector<uint32_t> &channels)
{
    auto found = backend.controls.find("channel_map");
    if (found == backend.controls.end())
        return 0;

    auto *control = mixer_get_ctl_by_name(mixer, found->second.c_str());
    if (!control)
        return -ENOENT;

    if (mixer_ctl_get_type(control) != MIXER_CTL_TYPE_INT ||
        mixer_ctl_get_num_values(control) < 2)
        return -EINVAL;

    std::vector<long> values(mixer_ctl_get_num_values(control));
    int status = mixer_ctl_get_array(control, values.data(), values.size());
    if (status < 0)
        return status == -1 ? -errno : status;

    if (values[0] <= 0 || size_t(values[0]) >= values.size() ||
        uint32_t(values[0]) != media.channels)
        return -EINVAL;

    std::vector<uint32_t> output;

    for (size_t i = 1; i <= size_t(values[0]); i++) {
        if (values[i] < 0 || values[i] > 255)
            return -ERANGE;

        output.push_back(values[i]);
    }

    channels = std::move(output);

    return 0;
}

}
