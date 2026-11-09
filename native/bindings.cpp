/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_bindings.h"

#include <algorithm>
#include <cerrno>
#include <new>
#include <stdexcept>
#include <json/json.h>

namespace ar_topology
{
namespace
{

uint32_t number(const Json::Value &value)
{
    if (!value.isUInt())
        throw std::invalid_argument("Invalid binding identity");

    return value.asUInt();
}

std::string text(const Json::Value &value)
{
    if (!value.isString() || value.asString().empty())
        throw std::invalid_argument("Invalid binding name");

    return value.asString();
}

Keys normalize(Keys keys)
{
    std::sort(keys.begin(), keys.end());

    for (size_t i = 1; i < keys.size(); i++) {
        if (keys[i - 1].first == keys[i].first)
            throw std::invalid_argument("Repeated graph key");
    }

    return keys;
}

uint32_t key_number(const Json::Value &value)
{
    std::string input = text(value);
    size_t end;
    unsigned long long result = std::stoull(input, &end, 0);
    if (input[0] < '0' || input[0] > '9' || end != input.size() ||
        result > UINT32_MAX)
        throw std::invalid_argument("Invalid graph key");

    return result;
}

Keys graph_keys(const Json::Value &input)
{
    if (!input.isArray())
        throw std::invalid_argument("Missing graph keys");

    Keys result;

    for (const auto &pair : input) {
        if (!pair.isArray() || pair.size() != 2)
            throw std::invalid_argument("Invalid graph key pair");

        result.emplace_back(key_number(pair[0]), key_number(pair[1]));
    }

    return normalize(std::move(result));
}

}

struct Bindings::Impl {
    struct Selection {
        std::set<uint32_t> graphs;
        std::set<std::string> references;
    };

    std::map<uint32_t, Graph> graphs;
    std::map<Keys, Selection> selections;
    std::map<uint32_t, Frontend> frontends;
    std::map<std::string, Reference> references;
    std::map<std::string, uint32_t> hardware;
};

Bindings::Bindings(const Json::Value &input) : impl(new Impl)
{
    if (input["version"] != 1 || !input["frontends"].isArray() ||
        !input["references"].isArray() || !input["selections"].isArray() ||
        !input["hardware_gates"].isArray())
        throw std::invalid_argument("Invalid topology bindings");

    std::map<uint32_t, uint32_t> subgraphs;
    std::map<std::string, uint32_t> controls;

    for (const auto &selection : input["selections"]) {
        Impl::Selection value;

        if (!selection["graphs"].isArray() ||
            !selection["tuning"].isArray() ||
            !selection["references"].isArray())
            throw std::invalid_argument("Invalid selection membership");

        for (const auto &graph : selection["graphs"]) {
            if (!value.graphs.insert(number(graph)).second)
                throw std::invalid_argument("Repeated graph membership");
        }

        std::set<uint32_t> tuned;

        for (const auto &graph : selection["tuning"]) {
            uint32_t id = number(graph["graph_id"]);
            Graph binding{number(graph["subgraph"]),
                           text(graph["set_control"])};

            if (!value.graphs.count(id) || !tuned.insert(id).second)
                throw std::invalid_argument("Tuning differs from membership");

            auto [prior, inserted] = impl->graphs.emplace(id, binding);
            if (!inserted && (prior->second.subgraph != binding.subgraph ||
                              prior->second.set_control != binding.set_control))
                throw std::invalid_argument("Conflicting graph binding");

            auto [sg, new_sg] = subgraphs.emplace(binding.subgraph, id);
            auto [ctl, new_ctl] = controls.emplace(binding.set_control, id);
            if ((!new_sg && sg->second != id) ||
                (!new_ctl && ctl->second != id))
                throw std::invalid_argument("Repeated graph identity");
        }

        if (tuned != value.graphs)
            throw std::invalid_argument("Incomplete tuning bindings");

        for (const auto &reference : selection["references"])
            value.references.insert(text(reference));

        auto [prior, inserted] = impl->selections.emplace(
            graph_keys(selection["graph_keys"]), value);
        if (!inserted && (prior->second.graphs != value.graphs ||
                          prior->second.references != value.references))
            throw std::invalid_argument("Conflicting graph key vector");
    }

    std::set<uint32_t> roots;
    std::set<std::string> switches;

    for (const auto &frontend : input["frontends"]) {
        uint32_t id = number(frontend["pcm_id"]);
        uint32_t root = number(frontend["graph_id"]);
        std::string direction = text(frontend["direction"]);
        std::string mode = text(frontend["mode"]);
        if (!impl->graphs.count(root) || !roots.insert(root).second ||
            (direction != "playback" && direction != "capture") ||
            (mode != "pcm" && mode != "hostless") ||
            !impl->frontends.emplace(id, Frontend{id, root,
                direction == "capture"}).second)
            throw std::invalid_argument("Invalid frontend binding");
    }

    for (const auto &reference : input["references"]) {
        Reference value{number(reference["source_graph"]),
                        number(reference["destination_graph"]), {}};
        std::string name = text(reference["name"]);

        if (!reference["control"].isBool() ||
            (reference["control"].asBool() &&
             reference.isMember("path_control")))
            throw std::invalid_argument("Invalid reference kind");

        if (reference.isMember("path_control")) {
            value.path = text(reference["path_control"]);

            if (controls.count(value.path) ||
                !switches.insert(value.path).second)
                throw std::invalid_argument("Repeated path control");
        }

        if (!impl->graphs.count(value.source) ||
            !impl->graphs.count(value.destination) ||
            controls.count(name) || !switches.insert(name).second ||
            !impl->references.emplace(name, value).second)
            throw std::invalid_argument("Invalid reference binding");
    }

    for (const auto &gate : input["hardware_gates"]) {
        uint32_t graph = number(gate["graph_id"]);
        std::string name = text(gate["control"]);
        if (!impl->graphs.count(graph) || roots.count(graph) ||
            controls.count(name) || !switches.insert(name).second)
            throw std::invalid_argument("Invalid hardware gate binding");

        impl->hardware.emplace(name, graph);
    }

    for (const auto &[keys, selection] : impl->selections) {
        (void)keys;

        for (const auto &reference : selection.references) {
            if (!impl->references.count(reference))
                throw std::invalid_argument("Unknown selection reference");
        }
    }
}

Bindings::~Bindings() = default;

int Bindings::select(const std::vector<Keys> &requests, Plan &plan) const
{
    try {
        Plan result;

        for (const auto &keys : requests) {
            auto found = impl->selections.find(normalize(keys));
            if (found == impl->selections.end())
                return -ENOENT;

            for (uint32_t id : found->second.graphs)
                result.graphs.emplace(id, impl->graphs.at(id));

            result.references.insert(found->second.references.begin(),
                                      found->second.references.end());
        }

        for (const auto &name : result.references) {
            const auto &reference = impl->references.at(name);
            if (!reference.path.empty() &&
                result.graphs.count(reference.source) &&
                result.graphs.count(reference.destination))
                result.paths.insert(reference.path);
        }

        std::set<uint32_t> owned;

        for (const auto &[id, frontend] : impl->frontends) {
            (void)id;

            if (!result.graphs.count(frontend.graph_id))
                continue;

            result.frontends.push_back(frontend);
            std::set<uint32_t> reached{frontend.graph_id};
            size_t previous;

            do {
                previous = reached.size();

                for (const auto &name : result.references) {
                    const auto &reference = impl->references.at(name);
                    if (!result.paths.count(reference.path))
                        continue;

                    uint32_t source = frontend.capture ? reference.destination :
                                                        reference.source;
                    uint32_t sink = frontend.capture ? reference.source :
                                                      reference.destination;

                    if (reached.count(source))
                        reached.insert(sink);
                }
            } while (previous != reached.size());

            owned.insert(reached.begin(), reached.end());
        }

        if (result.frontends.empty() || owned.size() != result.graphs.size())
            return -EOPNOTSUPP;

        for (const auto &[name, graph] : impl->hardware) {
            if (result.graphs.count(graph))
                result.hardware.insert(name);
        }

        plan = std::move(result);

        return 0;
    } catch (const std::invalid_argument &) {
        return -EINVAL;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

const std::map<std::string, Reference> &Bindings::references() const
{
    return impl->references;
}

}
