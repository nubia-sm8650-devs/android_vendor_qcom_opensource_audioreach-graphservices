/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef ACDB_TOPOLOGY_COMPILER_H
#define ACDB_TOPOLOGY_COMPILER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <json/json.h>
#include "source.h"

namespace acdb_topology
{

using Keys = std::vector<std::pair<uint32_t, uint32_t>>;

struct Compilation {
    Json::Value report;
    std::string topology;
};

Json::Value query_graph(const std::vector<std::string> &paths,
                       const Keys &keys);
Compilation compile_graph(const Source &graph, uint32_t graph_id);

}

#endif
