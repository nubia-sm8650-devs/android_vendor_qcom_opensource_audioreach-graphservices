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
    std::map<uint32_t, std::set<uint32_t>> modules;
    std::map<Keys, Selection> selections;
    std::map<std::pair<uint32_t, bool>, Frontend> frontends;
    std::map<std::string, Reference> references;
    std::map<std::string, uint32_t> hardware;
    std::map<std::string, Backend> backends;
    std::map<uint32_t, Backend> aifs;
};

Bindings::Bindings(const Json::Value &input) : impl(new Impl)
{
    if (input["version"] != 1 || !input["frontends"].isArray() ||
        !input["references"].isArray() || !input["selections"].isArray() ||
        !input["hardware_gates"].isArray())
        throw std::invalid_argument("Invalid topology bindings");

    std::map<uint32_t, uint32_t> subgraphs;
    std::map<uint32_t, uint32_t> module_owners;
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

            if (!graph["modules"].isArray())
                throw std::invalid_argument("Missing module ownership");

            std::set<uint32_t> modules;

            for (const auto &module : graph["modules"]) {
                uint32_t instance = number(module);
                auto [owner, added] = module_owners.emplace(instance, id);
                if (!modules.insert(instance).second ||
                    (!added && owner->second != id))
                    throw std::invalid_argument("Conflicting module owner");
            }

            auto [previous, added] = impl->modules.emplace(id, modules);
            if (!added && previous->second != modules)
                throw std::invalid_argument("Conflicting module membership");

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
    std::map<uint32_t, uint32_t> pcm_roots;
    std::set<std::string> switches;

    for (const auto &frontend : input["frontends"]) {
        uint32_t id = number(frontend["pcm_id"]);
        uint32_t root = number(frontend["graph_id"]);
        std::string direction = text(frontend["direction"]);
        std::string mode = text(frontend["mode"]);
        static const std::map<std::string, FrontendMode> modes{
            {"pcm", FrontendMode::pcm},
            {"hostless", FrontendMode::hostless},
            {"compress", FrontendMode::compress}};
        auto found = modes.find(mode);
        if (!impl->graphs.count(root) ||
            (direction != "playback" && direction != "capture") ||
            found == modes.end())
            throw std::invalid_argument("Invalid frontend binding");

        Frontend value{id, root, direction == "capture", found->second};
        auto [prior, added] = pcm_roots.emplace(id, root);
        auto other = impl->frontends.find({id, !value.capture});
        bool pair = !added && prior->second == root &&
            value.mode == FrontendMode::hostless &&
            other != impl->frontends.end() &&
            other->second.mode == FrontendMode::hostless;

        if ((!added && !pair) || (added && roots.count(root)) ||
            !impl->frontends.emplace(std::make_pair(id, value.capture),
                                     value).second)
            throw std::invalid_argument("Invalid frontend binding");

        roots.insert(root);
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

    std::map<BackendId, Backend> ports;
    std::map<std::string, BackendId> media_controls;
    std::map<std::string, uint32_t> aif_names;
    std::map<BackendId, uint32_t> aif_ports;

    for (const auto &gate : input["hardware_gates"]) {
        uint32_t graph = number(gate["graph_id"]);
        std::string name = text(gate["control"]);
        if (!impl->graphs.count(graph) || roots.count(graph) ||
            controls.count(name) || media_controls.count(name) ||
            !switches.insert(name).second)
            throw std::invalid_argument("Invalid hardware gate binding");

        impl->hardware.emplace(name, graph);

        if (gate.isMember("backend")) {
            const auto &native = gate["backend"];
            std::string direction = text(gate["direction"]);
            Backend backend{graph, number(gate["module"]),
                {text(native["provider"]), number(gate["dai"])},
                text(native["path"]), text(native["name"]),
                direction == "capture", {}, {}};
            if (number(native["dai"]) != backend.id.second ||
                (direction != "playback" && direction != "capture") ||
                !impl->modules.at(graph).count(backend.module) ||
                !native["controls"].isObject())
                throw std::invalid_argument("Invalid backend binding");

            for (const char *key : {"rate", "format", "data_format"}) {
                if (!native["controls"].isMember(key))
                    throw std::invalid_argument("Missing backend media control");
            }

            std::set<std::string> names;

            for (const auto &key : native["controls"].getMemberNames()) {
                std::string control = text(native["controls"][key]);
                if ((key != "rate" && key != "format" && key != "channels" &&
                     key != "data_format" && key != "channel_map") ||
                    control.size() >= 44 ||
                    controls.count(control) || switches.count(control) ||
                    !names.insert(control).second)
                    throw std::invalid_argument("Invalid backend media control");

                auto [prior, added] = media_controls.emplace(control, backend.id);
                if (!added && prior->second != backend.id)
                    throw std::invalid_argument("Conflicting backend control");

                backend.controls.emplace(key, std::move(control));
            }

            if (backend.controls.count("channels") &&
                backend.controls.count("channel_map"))
                throw std::invalid_argument("Conflicting channel sources");

            if (native.isMember("aif")) {
                if (!native["aif"].isObject())
                    throw std::invalid_argument("Invalid native AIF binding");

                for (const auto &key : native["aif"].getMemberNames()) {
                    if (key != "id" && key != "name")
                        throw std::invalid_argument("Unsupported native AIF field");
                }

                backend.aif_name = text(native["aif"]["name"]);
                backend.aif_id = number(native["aif"]["id"]);
                if (backend.aif_name.size() >= 32 ||
                    backend.aif_name.find('\0') != std::string::npos)
                    throw std::invalid_argument("AIF name is too long");

                auto [previous, added] = impl->aifs.emplace(backend.aif_id,
                                                            backend);
                if (!added && (previous->second.id != backend.id ||
                    previous->second.aif_name != backend.aif_name ||
                    previous->second.path != backend.path ||
                    previous->second.controls != backend.controls ||
                    previous->second.capture != backend.capture))
                    throw std::invalid_argument("Conflicting AIF identity");

                auto [named, new_name] = aif_names.emplace(backend.aif_name,
                                                           backend.aif_id);
                auto [port, new_port] = aif_ports.emplace(backend.id,
                                                          backend.aif_id);
                if ((!new_name && named->second != backend.aif_id) ||
                    (!new_port && port->second != backend.aif_id))
                    throw std::invalid_argument("Repeated AIF identity");
            }

            auto [prior, added] = ports.emplace(backend.id, backend);
            if (!added && (prior->second.controls != backend.controls ||
                           prior->second.capture != backend.capture))
                throw std::invalid_argument("Conflicting backend identity");

            impl->backends.emplace(name, std::move(backend));
        }
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
            auto normalized = normalize(keys);
            auto found = impl->selections.find(normalized);
            if (found == impl->selections.end())
                return -ENOENT;

            result.selections.emplace(normalized, found->second.graphs);

            for (uint32_t id : found->second.graphs) {
                result.graphs.emplace(id, impl->graphs.at(id));

                for (uint32_t instance : impl->modules.at(id))
                    result.modules.emplace(instance, id);
            }

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
            result.frontend_graphs[frontend.pcm_id].insert(reached.begin(),
                                                           reached.end());
        }

        if (result.frontends.empty() || owned.size() != result.graphs.size())
            return -EOPNOTSUPP;

        for (const auto &[name, graph] : impl->hardware) {
            if (result.graphs.count(graph)) {
                result.hardware.insert(name);

                auto backend = impl->backends.find(name);
                if (backend != impl->backends.end())
                    result.backends.emplace(*backend);
            }
        }

        plan = std::move(result);

        return 0;
    } catch (const std::invalid_argument &) {
        return -EINVAL;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int Bindings::frontends(const Keys &partial,
                        std::vector<Frontend> &frontends) const
{
    try {
        auto wanted = normalize(partial);
        auto same = [](const Frontend &a, const Frontend &b) {
            return a.pcm_id == b.pcm_id && a.graph_id == b.graph_id &&
                   a.capture == b.capture && a.mode == b.mode;
        };
        std::vector<Frontend> common;
        bool found = false;

        for (const auto &[keys, selection] : impl->selections) {
            (void)selection;

            if (!std::includes(keys.begin(), keys.end(), wanted.begin(),
                               wanted.end()))
                continue;

            Plan plan;

            int status = select({keys}, plan);
            if (status == -EOPNOTSUPP)
                continue;

            if (status)
                return status;

            if (found && !std::equal(common.begin(), common.end(),
                                     plan.frontends.begin(),
                                     plan.frontends.end(), same))
                return -EOPNOTSUPP;

            common = std::move(plan.frontends);
            found = true;
        }

        if (!found)
            return -ENOENT;

        frontends = std::move(common);

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

const std::map<uint32_t, Backend> &Bindings::aifs() const
{
    return impl->aifs;
}

}
