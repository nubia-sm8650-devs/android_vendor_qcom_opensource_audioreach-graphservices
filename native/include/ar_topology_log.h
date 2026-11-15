/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_LOG_H
#define AR_TOPOLOGY_LOG_H

#include "ar_topology.h"

#include <cstdio>
#include <string>
#include <vector>
#include <log/log.h>

#define LOG_ERRNO(status, ...) (ALOGE(__VA_ARGS__), (status))

namespace ar_topology
{

inline std::string text(const Keys &keys)
{
    std::string value;

    for (const auto &[key, item] : keys) {
        char pair[20];

        std::snprintf(pair, sizeof(pair), "%s%x=%x", value.empty() ? "" : " ",
                      key, item);
        value += pair;
    }

    return "[" + value + "]";
}

inline std::string text(const std::vector<Keys> &list)
{
    std::string value;

    for (const auto &keys : list)
        value += text(keys);

    return value;
}

}

#endif
