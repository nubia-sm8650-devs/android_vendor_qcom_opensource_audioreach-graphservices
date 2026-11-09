/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_BINDINGS_H
#define AR_TOPOLOGY_BINDINGS_H

#include "ar_topology.h"

#include <map>
#include <memory>
#include <set>
#include <utility>

namespace Json { class Value; }

namespace ar_topology
{

using Keys = std::vector<std::pair<uint32_t, uint32_t>>;

struct Frontend {
    uint32_t pcm_id;
    uint32_t graph_id;
    bool capture;
};

struct Reference {
    uint32_t source;
    uint32_t destination;
    std::string path;
};

using BackendId = std::pair<std::string, uint32_t>;

struct Backend {
    uint32_t graph, module;
    BackendId id;
    std::string path, name;
    bool capture;
    std::map<std::string, std::string> controls;
    std::string aif_name;
    uint32_t aif_id = 0;
};

struct Plan {
    std::map<uint32_t, Graph> graphs;
    std::map<uint32_t, uint32_t> modules;
    std::map<Keys, std::set<uint32_t>> selections;
    std::vector<Frontend> frontends;
    std::map<uint32_t, std::set<uint32_t>> frontend_graphs;
    std::set<std::string> references, paths, hardware;
    std::map<std::string, Backend> backends;
};

class Bindings {
public:
    explicit Bindings(const Json::Value &input);
    ~Bindings();
    int select(const std::vector<Keys> &requests, Plan &plan) const;
    const std::map<std::string, Reference> &references() const;
    const std::map<uint32_t, Backend> &aifs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}

#endif
