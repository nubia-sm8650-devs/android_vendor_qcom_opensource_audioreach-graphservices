/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef ACDB_TOPOLOGY_COMPILER_H
#define ACDB_TOPOLOGY_COMPILER_H

#include <cstdint>
#include <iosfwd>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <json/json.h>
#include "source.h"

namespace acdb_topology
{

using Keys = std::vector<std::pair<uint32_t, uint32_t>>;
using Tokens = std::vector<std::pair<uint32_t, uint32_t>>;

struct Topology {
    std::set<uint32_t> tokens;
    std::string body;

    std::string render() const;
};

struct Compilation {
    Json::Value report;
    Topology topology;
};

void append_word(Bytes &data, uint32_t value);
void private_data(std::ostream &out, const std::string &name,
                  uint32_t type, const Bytes &payload);
void control_data(std::ostream &out, const std::string &name,
                  const std::vector<Connection> &links);
void tuples(std::ostream &out, const std::string &name, const Tokens &tokens);
Json::Value parameter_controls(Topology &topology, uint32_t graph_id);
void dapm_switch(Topology &topology, const std::string &name,
                  uint32_t graph_id);
void bind_frontends(Compilation &compiled, const Json::Value &input);
Json::Value query_graph(const std::vector<std::string> &paths,
                       const Keys &keys);
std::vector<Json::Value> query_corpus(const std::vector<std::string> &paths);
Compilation compile_graph(const Source &graph, uint32_t graph_id,
                           const Json::Value &endpoints);
Compilation compile_corpus(const std::vector<Source> &sources,
                            const Json::Value &factored, uint32_t first_id,
                            const Json::Value &hardware_widgets = {},
                            const std::set<uint32_t> &excluded_modules = {});
Json::Value read_hardware_widgets(const Json::Value &input,
                                 const Json::Value &board);
void bind_hardware_widgets(Compilation &compiled, uint32_t graph_id,
                           const Json::Value &endpoints,
                           const Json::Value &widgets);
std::string module_widget_type(uint32_t module);
Json::Value factor_sources(const std::vector<Source> &sources);
void resolve_connections(Json::Value &factored);
void qualify_selections(Json::Value &factored, const Json::Value &contexts);
Json::Value read_unavailable(const Json::Value &input, const Json::Value &board);
Json::Value excluded_selections(const Json::Value &factored,
                                const Json::Value &unavailable);
Json::Value remaining_contexts(const Json::Value &contexts,
                               const Json::Value &excluded);
void describe_endpoints(Json::Value &factored, const Json::Value &board);
void bind_hardware(Json::Value &endpoints, const Json::Value &board);
Json::Value read_board(const std::string &resources,
                       const std::string &usecases, const std::string &dtb,
                       const Json::Value &links);

}

#endif
