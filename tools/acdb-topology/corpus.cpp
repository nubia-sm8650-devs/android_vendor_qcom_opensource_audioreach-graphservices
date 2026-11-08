/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <algorithm>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <sound/snd_ar_tokens.h>

namespace acdb_topology
{
namespace
{

using Objects = std::map<uint32_t, Bytes>;
using Records = std::map<uint32_t, std::vector<Bytes>>;
using RouteKey = std::tuple<bool, Bytes, uint32_t>;

struct Subgraph {
    Records records;
    Json::Value endpoints;
};

void append(Bytes &destination, const Bytes &bytes)
{
    destination.insert(destination.end(), bytes.begin(), bytes.end());
}

Bytes payload(const std::vector<Bytes> &objects)
{
    Bytes result;
    append_word(result, objects.size());

    for (const auto &object : objects)
        append(result, object);

    return result;
}

void issue(Json::Value &report, const std::string &kind, uint32_t object,
           uint32_t detail)
{
    Json::Value value;
    value["kind"] = kind;
    value["object"] = object;
    value["detail"] = detail;
    report["issues"].append(value);
}

Source source_for(uint32_t id, const Records &records)
{
    Json::Value original;
    Bytes data;

    for (const auto &[family, objects] : records) {
        uint32_t target = family == 0x08001162 ? 4 : 1;
        auto bytes = payload(objects);
        append_word(data, target);
        append_word(data, family);
        append_word(data, bytes.size());
        append_word(data, 0);
        append(data, bytes);
        data.resize((data.size() + 7) & ~size_t(7));
    }

    original["version"] = 1;
    original["canonical_subgraph"] = id;
    original["graph_keys"] = Json::Value(Json::arrayValue);
    original["subgraph_ids"] = Json::Value(Json::arrayValue);
    original["subgraph_ids"].append(std::to_string(id));
    original["graph_open"] = hex(data);
    original["graph_connections"] = "";
    original["driver_properties"] = "";
    original["subgraph_connections"] = "";

    return read_source(original);
}

Json::Value hardware(const Json::Value &endpoints, uint32_t subgraph)
{
    Json::Value result;
    result["physical"] = Json::Value(Json::arrayValue);

    for (const auto &endpoint : endpoints["physical"]) {
        if (endpoint["subgraph"].asUInt() != subgraph)
            continue;

        Json::Value value;
        value["module"] = endpoint["module"];
        value["binding"]["status"] = endpoint["binding"]["status"];

        if (endpoint["binding"]["status"] == "static-bound" ||
            endpoint["binding"]["status"] == "dai-configured") {
            value["binding"]["tokens"] = endpoint["binding"]["tokens"];
            value["binding"]["dai"] = endpoint["binding"]["dai"];
        }

        result["physical"].append(value);
    }

    return result;
}

void emit_reference(Topology &topology, const std::string &name,
                    bool control, const Connection &link)
{
    std::ostringstream out;

    if (control) {
        control_data(out, name + "_data", {link});
    } else {
        Tokens tokens{{AR_TKN_U32_MODULE_SRC_INSTANCE_ID, link.peers[0]},
                      {AR_TKN_U32_MODULE_SRC_OP_PORT_ID, link.peers[1]},
                      {AR_TKN_U32_MODULE_DST_INSTANCE_ID, link.peers[2]},
                      {AR_TKN_U32_MODULE_DST_IN_PORT_ID, link.peers[3]}};
        tuples(out, name + "_data", tokens);

        for (auto [token, value] : tokens) {
            (void)value;
            topology.tokens.insert(token);
        }
    }

    out << "SectionControlMixer.\"" << name << "\" {\n"
        << "    max \"1\"\n    no_pm \"true\"\n"
        << "    channel.\"fl\" { reg \"-1\" shift \"0\" }\n"
        << "    ops.\"ctl\" { get \"1\" put \"1\" info \"1\" }\n"
        << "    data [ \"" << name << "_data\" ]\n}\n\n";
    topology.body += out.str();
}

}

Compilation compile_corpus(const std::vector<Source> &sources,
                            const Json::Value &factored, uint32_t first_id)
{
    Compilation result;
    auto &report = result.report;
    report["issues"] = Json::Value(Json::arrayValue);
    report["subgraphs"] = Json::Value(Json::arrayValue);
    report["references"] = Json::Value(Json::arrayValue);
    report["selections"] = Json::Value(Json::arrayValue);
    report["runtime_bindings"] = false;
    std::map<uint32_t, uint32_t> owners;

    for (const auto &name : factored["module_ownership"].getMemberNames()) {
        const auto &candidates = factored["module_ownership"][name];
        if (candidates.size() != 1) {
            issue(report, "compiler-corpus-module-owner", read_number(name), 0);
            continue;
        }

        owners.emplace(read_number(name), candidates[0]["subgraph"].asUInt());
    }

    std::map<uint32_t, Subgraph> subgraphs;
    std::map<RouteKey, Connection> routes;
    std::vector<std::vector<RouteKey>> references;
    std::vector<std::set<uint32_t>> selections;

    for (size_t index = 0; index < sources.size(); index++) {
        const auto &source = sources[index];
        const auto &endpoints = factored["selections"]
                                        [Json::ArrayIndex(index)]["endpoints"];
        auto checked = compile_graph(source, first_id, endpoints);

        for (const auto &problem : checked.report["issues"]) {
            std::string kind = problem["kind"].asString();
            if (kind != "external-reference" &&
                kind != "external-control-reference" &&
                kind != "compiler-module-config")
                report["issues"].append(problem);
        }

        std::map<uint32_t, Objects> declarations;
        std::map<uint32_t, Records> current;
        std::map<uint32_t, std::set<uint32_t>> containers, modules;
        std::vector<Connection> data, control;

        for (const auto *parameters : {&source.open, &source.connections}) {
            for (const auto &parameter : *parameters) {
                Reader reader{parameter.payload};
                uint32_t family = parameter.id;
                if ((parameter.target == 1 &&
                     (family == 0x08001000 || family == 0x08001001 ||
                      family == 0x08001003)) ||
                    (parameter.target == 4 && family == 0x08001162)) {
                    for (const auto &object : read_declarations(reader))
                        declarations[family].emplace(object.id, object.bytes);
                } else if (parameter.target == 1 && family == 0x08001002) {
                    for (const auto &group : read_module_groups(reader)) {
                        auto sg = group.subgraph;
                        current[sg][family].push_back(group.bytes);
                        containers[sg].insert(group.container);

                        for (const auto &module : group.modules)
                            modules[sg].insert(module.instance);
                    }
                } else if (parameter.target == 1 &&
                           (family == 0x08001004 || family == 0x08001061)) {
                    auto &links = family == 0x08001004 ? data : control;
                    auto values = read_connections(reader,
                                                     family == 0x08001061);
                    links.insert(links.end(), values.begin(), values.end());
                }
            }
        }

        std::vector<RouteKey> selected_routes;

        for (bool is_control : {false, true}) {
            std::map<Bytes, uint32_t> occurrences;

            for (const auto &link : is_control ? control : data) {
                if (!owners.count(link.peers[0]) ||
                    !owners.count(link.peers[2])) {
                    issue(report, "compiler-corpus-unresolved-reference",
                          link.peers[0], link.peers[2]);
                    continue;
                }

                uint32_t left = owners.at(link.peers[0]);
                uint32_t right = owners.at(link.peers[2]);
                if (left == right) {
                    if (!current.count(left))
                        issue(report, "compiler-corpus-unselected-intrinsic",
                              left, link.peers[0]);
                    else
                        current[left][is_control ? 0x08001061 : 0x08001004]
                            .push_back(link.bytes);
                } else {
                    RouteKey key{is_control, link.bytes,
                                 occurrences[link.bytes]++};
                    routes.emplace(key, link);
                    selected_routes.push_back(key);
                }
            }
        }

        std::set<uint32_t> selected;

        for (const auto &[sg, declaration] : declarations[0x08001001]) {
            selected.insert(sg);
            auto &records = current[sg];
            records[0x08001001].push_back(declaration);

            for (uint32_t container : containers[sg])
                records[0x08001000].push_back(
                    declarations.at(0x08001000).at(container));

            for (uint32_t module : modules[sg])
                records[0x08001003].push_back(
                    declarations.at(0x08001003).at(module));

            if (declarations[0x08001162].count(sg))
                records[0x08001162].push_back(declarations[0x08001162].at(sg));

            for (auto &[family, objects] : records) {
                (void)family;
                std::sort(objects.begin(), objects.end());
            }

            Subgraph value{records, hardware(endpoints, sg)};
            auto [previous, inserted] = subgraphs.emplace(sg, value);
            if (!inserted) {
                if (previous->second.records != records)
                    issue(report, "compiler-corpus-subgraph-variant", sg,
                          index);

                if (previous->second.endpoints != value.endpoints)
                    issue(report, "compiler-corpus-hardware-variant", sg,
                          index);
            }
        }

        references.push_back(std::move(selected_routes));
        selections.push_back(std::move(selected));
    }

    if (subgraphs.size() > uint32_t(std::numeric_limits<int>::max()) ||
        first_id > uint32_t(std::numeric_limits<int>::max()) - subgraphs.size())
        throw std::runtime_error("Corpus graph IDs exceed the native range");

    std::map<uint32_t, uint32_t> graph_ids;
    uint32_t graph_id = first_id;

    for (const auto &[id, subgraph] : subgraphs) {
        graph_ids.emplace(id, graph_id);
        auto compiled = compile_graph(source_for(id, subgraph.records),
                                      graph_id++, subgraph.endpoints);
        compiled.report["subgraph"] = id;
        compiled.report["graph_id"] = graph_ids.at(id);
        report["subgraphs"].append(compiled.report);

        for (const auto &problem : compiled.report["issues"])
            report["issues"].append(problem);

        result.topology.tokens.insert(compiled.topology.tokens.begin(),
                                       compiled.topology.tokens.end());
        result.topology.body += compiled.topology.body;
    }

    std::map<RouteKey, std::string> names;

    for (const auto &[key, link] : routes) {
        bool control = std::get<0>(key);
        std::string name = std::string(control ? "ar_c" : "ar_d") +
                           std::to_string(names.size());
        names.emplace(key, name);

        uint32_t left = owners.at(link.peers[0]);
        uint32_t right = owners.at(link.peers[2]);
        if (!graph_ids.count(left) || !graph_ids.count(right)) {
            issue(report, "compiler-corpus-missing-subgraph", left, right);
            continue;
        }

        emit_reference(result.topology, name, control, link);
        Json::Value value;
        value["name"] = name;
        value["control"] = control;
        value["source_graph"] = graph_ids.at(left);
        value["destination_graph"] = graph_ids.at(right);
        value["native"] = hex(link.bytes);
        report["references"].append(value);
    }

    for (size_t index = 0; index < selections.size(); index++) {
        Json::Value value;
        value["graphs"] = Json::Value(Json::arrayValue);
        value["references"] = Json::Value(Json::arrayValue);

        for (uint32_t subgraph : selections[index])
            value["graphs"].append(graph_ids.at(subgraph));

        for (const auto &key : references[index])
            value["references"].append(names.at(key));

        report["selections"].append(value);
    }

    for (auto &subgraph : report["subgraphs"]) {
        for (const auto &problem : report["issues"]) {
            std::string kind = problem["kind"].asString();
            if ((kind == "compiler-corpus-subgraph-variant" ||
                 kind == "compiler-corpus-hardware-variant") &&
                problem["object"] == subgraph["subgraph"])
                subgraph["issues"].append(problem);
        }

        subgraph["can_emit"] = subgraph["issues"].empty();
    }

    report["can_emit"] = report["issues"].empty();

    if (!report["can_emit"].asBool())
        result.topology = {};

    return result;
}

}
