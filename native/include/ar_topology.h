/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_H
#define AR_TOPOLOGY_H

#include <cstdint>
#include <string>
#include <vector>

struct mixer;
struct pcm;

namespace ar_topology
{

struct Graph {
    uint32_t subgraph;
    std::string set_control;
};

int read_defaults(uint32_t subgraph, std::vector<uint8_t> &data);
int prepare(struct pcm *pcm, struct mixer *mixer,
             const std::vector<Graph> &new_graphs);

}

#endif
