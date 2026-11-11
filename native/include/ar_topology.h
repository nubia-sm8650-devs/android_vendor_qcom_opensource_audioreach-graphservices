/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_H
#define AR_TOPOLOGY_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct mixer;
struct pcm;

namespace ar_topology
{

using Keys = std::vector<std::pair<uint32_t, uint32_t>>;

struct Graph {
    uint32_t subgraph;
    std::string set_control;
    std::string get_control = {};
};

int read_defaults(uint32_t subgraph, std::vector<uint8_t> &data);
int read_calibration(uint32_t subgraph, const Keys &prior, const Keys &next,
                     std::vector<uint8_t> &data);
struct Calibration {
    Graph graph;
    Keys prior;
    Keys next;
};

int tune(struct mixer *mixer, const std::vector<Graph> &new_graphs,
         const Keys &prior = {}, const Keys &next = {});
int tune(struct mixer *mixer, const std::vector<Calibration> &calibrations);
int prepare(struct pcm *pcm, struct mixer *mixer,
             const std::vector<Graph> &new_graphs);
using Initialize = int (*)(void *) noexcept;
int prepare(struct pcm *pcm, struct mixer *mixer,
             const std::vector<Graph> &new_graphs,
             Initialize initialize, void *context);

}

#endif
