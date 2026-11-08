/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <array>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sound/snd_ar_tokens.h>

namespace acdb_topology
{
namespace
{

using Tokens = std::vector<std::pair<uint32_t, uint32_t>>;

struct Graph {
    std::map<uint32_t, Tokens> subgraphs, containers, ports;
    std::map<std::pair<uint32_t, uint32_t>, Bytes> definitions;
    std::vector<Module> modules;
    std::vector<std::array<uint32_t, 4>> edges;
    Json::Value issues{Json::arrayValue};

    void issue(const std::string &kind, uint32_t object, uint32_t detail)
    {
        Json::Value value;
        value["kind"] = kind;
        value["scope"] = kind.compare(0, 9, "compiler-") == 0 ?
                         "compiler-coverage" : "kernel-contract";
        value["object"] = object;
        value["detail"] = detail;
        issues.append(value);
    }

    void properties(Reader &reader, uint32_t family)
    {
        const std::map<uint32_t, uint32_t> sg_tokens = {
            {0x0800100e, AR_TKN_U32_SUB_GRAPH_PERF_MODE},
            {0x0800100f, AR_TKN_U32_SUB_GRAPH_DIRECTION},
            {0x08001010, AR_TKN_U32_SUB_GRAPH_SCENARIO_ID},
        };
        const std::map<uint32_t, uint32_t> container_tokens = {
            {0x08001011, AR_TKN_U32_CONTAINER_CAPABILITY_ID},
            {0x08001012, AR_TKN_U32_CONTAINER_GRAPH_POS},
            {0x08001013, AR_TKN_U32_CONTAINER_STACK_SIZE},
            {0x08001014, AR_TKN_U32_CONTAINER_PROC_DOMAIN},
        };
        auto &objects = family == 0x08001001 ? subgraphs :
                        family == 0x08001000 ? containers : ports;
        for (const auto &declaration : read_declarations(reader)) {
            uint32_t id = declaration.id;
            Tokens tokens;
            std::set<uint32_t> seen;

            for (const auto &entry : declaration.properties) {
                uint32_t property = entry.id;
                Reader data{entry.payload};

                if (!seen.insert(property).second)
                    throw std::runtime_error("Repeated graph property");

                if (family == 0x08001003 && property == 0x08001015) {
                    tokens.emplace_back(AR_TKN_U32_MODULE_MAX_IP_PORTS,
                                        data.word());
                    tokens.emplace_back(AR_TKN_U32_MODULE_MAX_OP_PORTS,
                                        data.word());
                    data.finish();
                    continue;
                }

                const auto &known = family == 0x08001001 ? sg_tokens :
                                    container_tokens;
                auto token = known.find(property);
                if (family == 0x08001003 || token == known.end()) {
                    issue("property", id, property);
                    continue;
                }

                if (property == 0x08001011 && data.word() != 1) {
                    issue("container-capabilities", id, property);
                    continue;
                }

                tokens.emplace_back(token->second, data.word());
                data.finish();
            }

            auto [previous, inserted] = definitions.emplace(
                std::make_pair(family, id), declaration.bytes);
            if (!inserted && previous->second != declaration.bytes)
                throw std::runtime_error("Conflicting graph declaration");

            objects.emplace(id, tokens);

            size_t required = family == 0x08001001 ? 3 :
                              family == 0x08001000 ? 4 : 2;

            if (tokens.size() != required)
                issue("missing-properties", id, family);
        }
    }

    void records(const std::vector<Parameter> &parameters)
    {
        for (const auto &parameter : parameters) {
            uint32_t target = parameter.target, family = parameter.id;
            Reader data{parameter.payload};

            if (target != 1) {
                issue("parameter-target", target, family);
                continue;
            }

            switch (family) {
            case 0x08001000:
            case 0x08001001:
            case 0x08001003:
                properties(data, family);
                break;
            case 0x08001002:
                for (const auto &group : read_module_groups(data))
                    modules.insert(modules.end(), group.modules.begin(),
                                   group.modules.end());
                break;
            case 0x08001004:
                for (const auto &connection : read_connections(data, false))
                    edges.push_back(connection.peers);
                break;
            default:
                issue("parameter", target, family);
                continue;
            }

            data.finish();
        }
    }
};

void tuples(std::ostream &out, const std::string &name,
            const Tokens &tokens)
{
    out << "SectionVendorTuples.\"" << name << "\" {\n"
        << "    tokens \"ar\"\n    tuples.\"word\" {\n";

    for (auto [token, value] : tokens)
        out << "        t" << token << " \"" << value << "\"\n";

    out << "    }\n}\nSectionData.\"" << name << "\" {\n"
        << "    tuples \"" << name << "\"\n}\n\n";
}

}

Compilation compile_graph(const Source &input, uint32_t graph_id)
{
    const auto &source = input.original;
    if (source["version"] != 1 || !source["subgraph_ids"].isArray() ||
        !source["graph_keys"].isArray())
        throw std::runtime_error("Unsupported graph export");

    if (graph_id > uint32_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("Graph ID exceeds the native DAI ID range");

    Graph graph;
    unhex(source["subgraph_connections"]);
    unhex(source["driver_properties"]);
    graph.records(input.open);
    graph.records(input.connections);
    std::set<uint32_t> expected;

    for (const auto &value : source["subgraph_ids"]) {
        if (!value.isString())
            throw std::runtime_error("Invalid subgraph identity");

        std::string text = value.asString();
        size_t end;
        unsigned long long id = std::stoull(text, &end, 0);
        if (text.empty() || text[0] < '0' || text[0] > '9' ||
            end != text.size() || id > UINT32_MAX ||
            !expected.insert(id).second)
            throw std::runtime_error("Invalid subgraph identity");
    }

    std::set<uint32_t> declared;

    for (const auto &[id, tokens] : graph.subgraphs) {
        (void)tokens;
        declared.insert(id);
    }

    if (expected != declared)
        throw std::runtime_error(
            "Subgraph membership differs from declarations");

    if (expected.empty())
        graph.issue("empty-graph", graph_id, 0);

    std::map<uint32_t, const Module *> modules;
    std::map<uint32_t, uint32_t> containers;
    std::set<uint32_t> used_subgraphs;

    auto capacity = [&](uint32_t instance) {
        for (auto [token, value] : graph.ports.at(instance)) {
            if (token == AR_TKN_U32_MODULE_MAX_OP_PORTS)
                return value;
        }

        return uint32_t(0);
    };

    for (const auto &module : graph.modules) {
        if (!modules.emplace(module.instance, &module).second ||
            !graph.subgraphs.count(module.subgraph) ||
            !graph.containers.count(module.container) ||
            !graph.ports.count(module.instance))
            throw std::runtime_error("Invalid module declaration");

        used_subgraphs.insert(module.subgraph);

        auto [owner, inserted] = containers.emplace(module.container,
                                                     module.subgraph);
        if (!inserted && owner->second != module.subgraph)
            graph.issue("shared-container", module.container, module.subgraph);

        if (module_widget_type(module.type) == "buffer")
            graph.issue("compiler-module-config", module.instance,
                        module.type);
    }

    std::map<uint32_t, std::vector<std::array<uint32_t, 4>>> outgoing;

    for (const auto &edge : graph.edges) {
        if (!modules.count(edge[0]) || !modules.count(edge[2]))
            graph.issue("external-reference", edge[0], edge[2]);

        if (!edge[1] || !edge[3])
            graph.issue("zero-port", edge[0], edge[2]);

        outgoing[edge[0]].push_back(edge);
    }

    for (const auto &[instance, edges] : outgoing) {
        std::set<uint32_t> ports;
        for (const auto &edge : edges)
            ports.insert(edge[1]);

        if (modules.count(instance) && ports.size() > capacity(instance))
            graph.issue("port-capacity", instance, ports.size());

        if (edges.size() > 8)
            graph.issue("connection-slots", instance, edges.size());
    }

    if (modules.size() != graph.ports.size() ||
        containers.size() != graph.containers.size() ||
        used_subgraphs != declared)
        graph.issue("orphan-declaration", graph_id, 0);

    Compilation result;
    result.report["source"] = source;
    result.report["issues"] = graph.issues;
    result.report["subgraphs"] = Json::UInt64(graph.subgraphs.size());
    result.report["containers"] = Json::UInt64(graph.containers.size());
    result.report["modules"] = Json::UInt64(graph.modules.size());
    result.report["data_links"] = Json::UInt64(graph.edges.size());
    result.report["can_emit"] = graph.issues.empty();
    result.report["runtime_bindings"] = false;

    if (!graph.issues.empty())
        return result;

    std::ostringstream body;
    std::set<uint32_t> used_tokens;

    auto emit = [&](const std::string &name, const Tokens &tokens) {
        for (auto [token, value] : tokens) {
            (void)value;
            used_tokens.insert(token);
        }

        tuples(body, name, tokens);
    };

    for (const auto &module : graph.modules) {
        std::string name = "m" + std::to_string(module.instance);
        Tokens sg = {{AR_TKN_U32_SUB_GRAPH_INSTANCE_ID, module.subgraph},
                     {AR_TKN_DAI_INDEX, graph_id}};
        Tokens container = {{AR_TKN_U32_CONTAINER_INSTANCE_ID,
                             module.container}};
        Tokens tokens = {{AR_TKN_U32_MODULE_INSTANCE_ID, module.instance},
                         {AR_TKN_U32_MODULE_ID, module.type}};

        const auto &sg_properties = graph.subgraphs.at(module.subgraph);
        const auto &ct_properties = graph.containers.at(module.container);
        const auto &ports = graph.ports.at(module.instance);
        sg.insert(sg.end(), sg_properties.begin(), sg_properties.end());
        container.insert(container.end(), ct_properties.begin(),
                         ct_properties.end());
        tokens.insert(tokens.end(), ports.begin(), ports.end());
        unsigned index = 0;

        for (const auto &edge : outgoing[module.instance]) {
            uint32_t src = index ? 207 + index * 3 :
                                  AR_TKN_U32_MODULE_SRC_OP_PORT_ID;
            uint32_t dst = index ? src + 1 :
                                  AR_TKN_U32_MODULE_DST_IN_PORT_ID;
            uint32_t instance = index ? src + 2 :
                                       AR_TKN_U32_MODULE_DST_INSTANCE_ID;
            tokens.emplace_back(src, edge[1]);
            tokens.emplace_back(dst, edge[3]);
            tokens.emplace_back(instance, edge[2]);
            index++;
        }

        emit(name + "_sg", sg);
        emit(name + "_container", container);
        emit(name + "_module", tokens);
        body << "SectionWidget.\"" << name << "\" {\n    type \""
             << module_widget_type(module.type) << "\"\n    no_pm \"true\"\n"
             << "    data [ \"" << name << "_sg\" \"" << name
             << "_container\" \"" << name << "_module\" ]\n}\n\n";
    }

    std::ostringstream out;
    out << "SectionVendorTokens.\"ar\" {\n";

    for (uint32_t token : used_tokens)
        out << "    t" << token << " \"" << token << "\"\n";

    out << "}\n\n" << body.str();
    result.topology = out.str();

    return result;
}

}
