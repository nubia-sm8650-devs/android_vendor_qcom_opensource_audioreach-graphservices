/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_MEDIA_H
#define AR_TOPOLOGY_MEDIA_H

#include "ar_topology_bindings.h"

namespace ar_topology
{

struct Media {
    uint32_t rate, channels, data_format;
    std::string format;

    bool operator==(const Media &other) const
    {
        return rate == other.rate && channels == other.channels &&
            data_format == other.data_format && format == other.format;
    }
};

struct HardwareConfig {
    Keys selection;
    Media media;
};

}

#endif
