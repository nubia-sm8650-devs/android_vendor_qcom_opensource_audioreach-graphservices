/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <array>
#include <map>
#include <set>

namespace acdb_topology
{
namespace
{

using Owner = std::array<uint32_t, 3>;
using Ownership = std::map<uint32_t, Owner>;
using Candidates = std::map<uint32_t, std::set<Owner>>;

Json::Value owner_json(const Owner &owner)
{
    Json::Value result;
    result["subgraph"] = owner[0];
    result["container"] = owner[1];
    result["type"] = owner[2];

    return result;
}

Json::Value endpoint(uint32_t module, uint32_t port,
                     const Ownership &selected, const Candidates &corpus)
{
    Json::Value result;
    result["module"] = module;
    result["port"] = port;
    result["owner"] = Json::Value();
    auto local = selected.find(module);
    result["selected"] = local != selected.end();

    if (local != selected.end()) {
        result["owner"] = owner_json(local->second);
        result["resolution"] = "selection";

        return result;
    }

    auto candidates = corpus.find(module);
    if (candidates == corpus.end())
        result["resolution"] = "unknown";
    else if (candidates->second.size() != 1)
        result["resolution"] = "ambiguous";
    else {
        result["owner"] = owner_json(*candidates->second.begin());
        result["resolution"] = "corpus";
    }

    return result;
}

Json::Value requirement(const Json::Value &link, const Ownership &selected,
                         const Candidates &corpus,
                         const std::set<uint32_t> &selected_subgraphs)
{
    const auto &peers = link["peers"];
    Json::Value result;
    result["source"] = endpoint(peers[0].asUInt(), peers[1].asUInt(),
                                selected, corpus);
    result["destination"] = endpoint(peers[2].asUInt(), peers[3].asUInt(),
                                     selected, corpus);
    const auto &source = result["source"];
    const auto &destination = result["destination"];
    bool complete = !source["owner"].isNull() &&
                    !destination["owner"].isNull();
    result["ownership_complete"] = complete;
    result["endpoints_selected"] = source["selected"].asBool() &&
                                   destination["selected"].asBool();
    result["unselected_modules"] = Json::Value(Json::arrayValue);
    std::set<uint32_t> unselected;

    for (const auto *end : {&source, &destination}) {
        if (!(*end)["selected"].asBool())
            unselected.insert((*end)["module"].asUInt());
    }

    for (uint32_t module : unselected)
        result["unselected_modules"].append(module);

    result["required_subgraphs"] = Json::Value();
    result["missing_subgraphs"] = Json::Value();
    result["relation"] = "unresolved";

    if (!complete)
        return result;

    const auto &left = source["owner"];
    const auto &right = destination["owner"];
    std::set<uint32_t> required{left["subgraph"].asUInt(),
                                right["subgraph"].asUInt()};
    result["required_subgraphs"] = Json::Value(Json::arrayValue);
    result["missing_subgraphs"] = Json::Value(Json::arrayValue);

    for (uint32_t subgraph : required) {
        result["required_subgraphs"].append(subgraph);

        if (!selected_subgraphs.count(subgraph))
            result["missing_subgraphs"].append(subgraph);
    }

    if (left["subgraph"] == right["subgraph"])
        result["relation"] = "same-subgraph";
    else if (left["container"] == right["container"])
        result["relation"] = "shared-container";
    else
        result["relation"] = "cross-container";

    return result;
}

}

void resolve_connections(Json::Value &factored)
{
    const auto &definitions = factored["definitions"];
    Candidates corpus;

    for (const auto &group : definitions["module_groups"]) {
        for (const auto &module : group["modules"])
            corpus[module["id"].asUInt()].insert({group["subgraph"].asUInt(),
                group["container"].asUInt(), module["type"].asUInt()});
    }

    factored["module_ownership"] = Json::Value(Json::objectValue);

    for (const auto &[module, owners] : corpus) {
        Json::Value values(Json::arrayValue);

        for (const auto &owner : owners)
            values.append(owner_json(owner));

        factored["module_ownership"][std::to_string(module)] = values;
    }

    for (auto &selection : factored["selections"]) {
        Ownership selected;
        std::set<uint32_t> subgraphs;

        for (const char *buffer : {"graph_open", "graph_connections"}) {
            for (const auto &record : selection[buffer]) {
                std::string kind = record.get("kind", "").asString();

                for (const auto &reference : record["items"]) {
                    if (kind == "subgraphs")
                        subgraphs.insert(definitions[kind][reference.asUInt()]
                                          ["id"].asUInt());

                    if (kind != "module_groups")
                        continue;

                    const auto &group = definitions[kind][reference.asUInt()];

                    for (const auto &module : group["modules"])
                        selected.emplace(module["id"].asUInt(), Owner{
                            group["subgraph"].asUInt(),
                            group["container"].asUInt(),
                            module["type"].asUInt()});
                }
            }
        }

        Json::Value requirements(Json::arrayValue);

        for (const char *buffer : {"graph_open", "graph_connections"}) {
            const auto &records = selection[buffer];

            for (Json::ArrayIndex index = 0; index < records.size(); index++) {
                const auto &record = records[index];
                std::string kind = record.get("kind", "").asString();
                if (kind != "data_links" && kind != "control_links")
                    continue;

                const auto &items = record["items"];

                for (Json::ArrayIndex item = 0; item < items.size(); item++) {
                    uint32_t reference = items[item].asUInt();
                    auto value = requirement(definitions[kind][reference],
                                               selected, corpus, subgraphs);
                    value["kind"] = kind;
                    value["definition"] = reference;
                    value["buffer"] = buffer;
                    value["record"] = index;
                    value["item"] = item;
                    requirements.append(value);
                }
            }
        }

        selection["connection_requirements"] = std::move(requirements);
    }
}

}
