/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace acdb_topology
{
namespace
{

constexpr uint32_t write_shared_memory = 0x07001000;
constexpr uint32_t placeholder_encoder = 0x07001008;
constexpr uint32_t placeholder_decoder = 0x07001009;

struct Root {
    uint32_t graph;
    std::map<uint32_t, uint32_t> modules;
    bool hardware;
};

uint32_t hostless_module(const Root &root, bool playback,
                         const std::set<std::pair<uint32_t, uint32_t>> &edges)
{
    if (root.modules.size() == 1)
        return root.modules.begin()->first;

    std::set<uint32_t> candidates;

    for (const auto &[instance, type] : root.modules) {
        (void)type;
        bool input = false, output = false;

        for (const auto &[source, sink] : edges) {
            input |= sink == instance;
            output |= source == instance;
        }

        if (playback ? output && !input : input && !output)
            candidates.insert(instance);
    }

    if (candidates.size() != 1)
        throw std::runtime_error("Hostless PCM root needs an explicit module");

    return *candidates.begin();
}

void capabilities(std::ostream &out, const std::string &name,
                   const Json::Value &input)
{
    const std::set<std::string> supported_formats{
        "S16_LE", "S24_LE", "S24_3LE", "S32_LE"};
    const std::set<uint32_t> supported_rates{
        8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000,
        88200, 96000, 176400, 192000};

    if (!input["formats"].isArray() || input["formats"].empty() ||
        !input["rates"].isArray() || input["rates"].empty() ||
        !input["channels_min"].isUInt() ||
        !input["channels_max"].isUInt())
        throw std::runtime_error("Invalid PCM capabilities");

    std::set<std::string> formats;
    std::set<uint32_t> rates;

    for (const auto &format : input["formats"]) {
        if (!format.isString() ||
            !supported_formats.count(format.asString()) ||
            !formats.insert(format.asString()).second)
            throw std::runtime_error("Unsupported or repeated PCM format");
    }

    for (const auto &rate : input["rates"]) {
        if (!rate.isUInt() || !supported_rates.count(rate.asUInt()) ||
            !rates.insert(rate.asUInt()).second)
            throw std::runtime_error("Unsupported or repeated PCM rate");
    }

    uint32_t minimum = input["channels_min"].asUInt();
    uint32_t maximum = input["channels_max"].asUInt();
    if (!minimum || minimum > maximum || maximum > 8)
        throw std::runtime_error("Unsupported PCM channel range");

    out << "SectionPCMCapabilities.\"" << name << "\" {\n    formats \"";
    std::string separator;

    for (const auto &format : formats) {
        out << separator << format;
        separator = ",";
    }

    out << "\"\n    rates \"";
    separator.clear();

    for (uint32_t rate : rates) {
        out << separator << rate;
        separator = ",";
    }

    out << "\"\n    rate_min \"" << *rates.begin() << "\"\n"
        << "    rate_max \"" << *rates.rbegin() << "\"\n"
        << "    channels_min \"" << minimum << "\"\n"
        << "    channels_max \"" << maximum << "\"\n}\n\n";
}

void line(std::ostream &out, const std::string &sink,
           const std::string &control, const std::string &source)
{
    out << "        \"" << sink << ", " << control << ", " << source
        << "\"\n";
}

}

void bind_frontends(Compilation &compiled, const Json::Value &input)
{
    if (!input.isObject() || input["version"] != 1 ||
        !input["pcms"].isArray())
        throw std::runtime_error("Invalid PCM bindings");

    if (!compiled.report["can_emit"].asBool())
        return;

    auto &report = compiled.report;
    std::map<uint32_t, Root> roots;
    std::map<uint32_t, Json::Value> tuning;
    std::set<std::pair<uint32_t, uint32_t>> edges;

    for (const auto &subgraph : report["subgraphs"]) {
        uint32_t id = subgraph["subgraph"].asUInt();
        Root root{subgraph["graph_id"].asUInt(), {},
                  !subgraph["hardware_gates"].empty()};
        Json::Value calibration;
        calibration["subgraph"] = id;
        calibration["graph_id"] = root.graph;
        calibration["set_control"] = subgraph["parameter_controls"]["set"];
        Source source = read_source(subgraph["source"]);

        for (const auto &parameter : source.open) {
            Reader reader{parameter.payload};

            if (parameter.target != 1)
                continue;

            if (parameter.id == 0x08001002) {
                for (const auto &group : read_module_groups(reader)) {
                    for (const auto &module : group.modules)
                        root.modules.emplace(module.instance, module.type);
                }
            } else if (parameter.id == 0x08001004) {
                for (const auto &link : read_connections(reader, false))
                    edges.emplace(link.peers[0], link.peers[2]);
            }
        }

        calibration["modules"] = Json::Value(Json::arrayValue);

        for (const auto &[instance, type] : root.modules) {
            (void)type;
            calibration["modules"].append(instance);
        }

        tuning.emplace(root.graph, calibration);
        roots.emplace(id, std::move(root));
    }

    report["frontends"] = Json::Value(Json::arrayValue);
    std::set<uint32_t> pcm_ids, root_ids;
    std::ostringstream out, paths;
    Topology switches;

    for (const auto &binding : input["pcms"]) {
        if (!binding["id"].isUInt() || !binding["subgraph"].isString() ||
            !binding["direction"].isString() || !binding["mode"].isString())
            throw std::runtime_error("Invalid PCM binding");

        uint32_t pcm = binding["id"].asUInt();
        uint32_t sg = read_number(binding["subgraph"].asString());
        std::string direction = binding["direction"].asString();
        std::string mode = binding["mode"].asString();
        if (pcm > uint32_t(std::numeric_limits<int32_t>::max()) ||
            !roots.count(sg) || !pcm_ids.insert(pcm).second ||
            !root_ids.insert(sg).second ||
            (direction != "playback" && direction != "capture") ||
            (mode != "pcm" && mode != "hostless" && mode != "compress"))
            throw std::runtime_error("Unsupported or repeated PCM binding");

        const auto &root = roots.at(sg);
        bool playback = direction == "playback";
        uint32_t module = 0;
        unsigned hosts = 0;
        bool decoder = false, encoder = false;

        for (auto [instance, type] : root.modules) {
            decoder |= type == placeholder_decoder;
            encoder |= type == placeholder_encoder;

            std::string role = module_widget_type(type);
            if (role != "aif_in" && role != "aif_out")
                continue;

            hosts++;

            if (role == (playback ? "aif_in" : "aif_out"))
                module = instance;
        }

        if (mode == "hostless") {
            if (hosts || (binding.isMember("module") &&
                          (!binding["module"].isUInt() ||
                           !root.modules.count(binding["module"].asUInt()))))
                throw std::runtime_error("Invalid hostless PCM root");

            module = binding.isMember("module") ?
                binding["module"].asUInt() :
                hostless_module(root, playback, edges);
        } else if (hosts != 1 || !module || binding.isMember("module")) {
            throw std::runtime_error("PCM root lacks a unique host endpoint");
        } else if (mode == "pcm" && (decoder || encoder)) {
            throw std::runtime_error("A codec graph needs a compressed binding");
        } else if (mode == "compress" &&
                   (!playback || !decoder ||
                    root.modules.at(module) != write_shared_memory)) {
            throw std::runtime_error("Unsupported compressed PCM root");
        }

        if (root.hardware)
            throw std::runtime_error("A hardware graph cannot own a frontend");

        std::string name = "ar_pcm" + std::to_string(pcm);
        std::string dai = "ar_fe" + std::to_string(root.graph);
        std::string stream = dai + "_" + direction;
        capabilities(out, stream, binding["capabilities"]);
        out << "SectionPCM.\"" << name << "\" {\n"
            << "    id \"" << pcm << "\"\n"
            << (mode == "compress" ? "    compress \"1\"\n" : "")
            << "    dai.\"" << dai << "\" { id \"" << root.graph << "\" }\n"
            << "    pcm.\"" << direction << "\" { capabilities \"" << stream
            << "\" }\n}\n\n";
        std::string node = "m" + std::to_string(module);

        if (direction == "playback")
            line(paths, node, "", stream);
        else
            line(paths, stream, "", node);

        Json::Value frontend;
        frontend["pcm_id"] = pcm;
        frontend["subgraph"] = sg;
        frontend["graph_id"] = root.graph;
        frontend["module"] = module;
        frontend["mode"] = mode;
        frontend["direction"] = direction;
        frontend["name"] = name;
        frontend["dai_name"] = dai;
        frontend["stream_widget"] = stream;
        report["frontends"].append(frontend);
    }

    for (const auto &[source, sink] : edges)
        line(paths, "m" + std::to_string(sink), "", "m" +
             std::to_string(source));

    for (auto &reference : report["references"]) {
        if (reference["control"].asBool())
            continue;

        std::string marker = "ar_p_" + reference["name"].asString();
        std::string control = marker + "_on";
        dapm_switch(switches, marker,
                    reference["destination_graph"].asUInt());
        Reader reader{unhex(reference["native"])};
        uint32_t source = reader.word();
        reader.word();
        uint32_t sink = reader.word();
        reader.word();
        reader.finish();
        line(paths, marker, control, "m" + std::to_string(source));
        line(paths, "m" + std::to_string(sink), "", marker);
        reference["path_control"] = marker + " " + control;
    }

    for (auto &selection : report["selections"]) {
        std::set<uint32_t> selected;
        selection["frontends"] = Json::Value(Json::arrayValue);
        selection["paths"] = Json::Value(Json::arrayValue);
        selection["tuning"] = Json::Value(Json::arrayValue);

        for (const auto &graph : selection["graphs"])
            selected.insert(graph.asUInt());

        for (uint32_t graph : selected)
            selection["tuning"].append(tuning.at(graph));

        for (const auto &frontend : report["frontends"]) {
            if (selected.count(frontend["graph_id"].asUInt()))
                selection["frontends"].append(frontend["pcm_id"]);
        }

        for (const auto &name : selection["references"]) {
            for (const auto &reference : report["references"]) {
                if (reference["name"] == name &&
                    reference.isMember("path_control") &&
                    selected.count(reference["source_graph"].asUInt()) &&
                    selected.count(reference["destination_graph"].asUInt()))
                    selection["paths"].append(reference["path_control"]);
            }
        }

        std::set<uint32_t> owned;

        for (const auto &frontend : report["frontends"]) {
            uint32_t root = frontend["graph_id"].asUInt();
            if (!selected.count(root))
                continue;

            bool playback = frontend["direction"] == "playback";
            std::set<uint32_t> reached{root};
            size_t previous;

            do {
                previous = reached.size();

                for (const auto &reference : report["references"]) {
                    if (!reference.isMember("path_control"))
                        continue;

                    uint32_t source = reference[playback ? "source_graph" :
                                                       "destination_graph"]
                                      .asUInt();
                    uint32_t sink = reference[playback ? "destination_graph" :
                                                       "source_graph"].asUInt();
                    bool enabled = false;

                    for (const auto &path : selection["paths"])
                        enabled |= path == reference["path_control"];

                    if (enabled && reached.count(source))
                        reached.insert(sink);
                }
            } while (previous != reached.size());

            owned.insert(reached.begin(), reached.end());
        }

        selection["unowned_graphs"] = Json::Value(Json::arrayValue);

        for (uint32_t graph : selected) {
            if (!owned.count(graph))
                selection["unowned_graphs"].append(graph);
        }

        selection["ownership_bound"] = !selection["frontends"].empty() &&
                                         selection["unowned_graphs"].empty();
    }

    out << "SectionGraph.\"ar_frontends\" {\n    index \"0\"\n"
        << "    lines [\n" << paths.str() << "    ]\n}\n\n";

    compiled.topology.tokens.insert(switches.tokens.begin(),
                                    switches.tokens.end());
    compiled.topology.body = switches.body + compiled.topology.body + out.str();
}

}
