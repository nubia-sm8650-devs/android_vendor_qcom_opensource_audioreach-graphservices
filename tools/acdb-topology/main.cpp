/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

using acdb_topology::read_number;

Json::Value read_json(const fs::path &path)
{
    std::ifstream input(path);
    Json::Value value;
    Json::CharReaderBuilder builder;
    std::string error;
    builder["rejectDupKeys"] = true;

    if (!input || !Json::parseFromStream(builder, input, &value, &error))
        throw std::runtime_error("Cannot read JSON " + path.string() + error);

    return value;
}

void write(const std::string &path, const std::string &data)
{
    std::ofstream output(path);
    if (!output || !(output << data) || !output.flush())
        throw std::runtime_error("Cannot write " + path);
}

int main(int argc, char **argv)
{
    try {
        std::vector<std::string> databases;
        acdb_topology::Keys keys;
        std::set<uint32_t> key_ids;
        std::string exports, input, output, report;
        std::string resources, usecases, dtb, selection_contexts;
        std::string hardware_widgets, pcm_bindings, bindings_output;
        Json::Value context(Json::objectValue), devices(Json::arrayValue);
        uint32_t graph_id = 0;
        bool corpus = false;

        for (int i = 1; i < argc; i++) {
            std::string option = argv[i];
            if (option == "--help") {
                std::cout << "Usage: ar-acdb-topology --report FILE\n"
                          << "  (--acdb FILE ... --key ID=VALUE ... |\n"
                          << "   --input EXPORT.json | --exports DIRECTORY)\n"
                          << "  [--graph-id ID] [--output TOPOLOGY.conf]\n"
                          << "  [--corpus]\n"
                          << "  [--resource-manager XML --usecase-kv XML\n"
                          << "   --board-dtb DTB]\n"
                          << "  [--device PAL_DEVICE ... --stream-type TYPE\n"
                          << "   --device-pp-type TYPE\n"
                          << "   --context NAME=VALUE ...]\n"
                          << "  [--selection-contexts FILE]\n"
                          << "  [--hardware-widgets FILE]\n"
                          << "  [--pcm-bindings FILE] [--bindings-output FILE]\n";
                return 0;
            }

            if (option == "--corpus") {
                corpus = true;
                continue;
            }

            if (++i == argc)
                throw std::runtime_error("Missing value for " + option);

            std::string value = argv[i];

            if (option == "--acdb")
                databases.push_back(value);
            else if (option == "--key") {
                size_t equal = value.find('=');
                if (equal == std::string::npos)
                    throw std::runtime_error("Expected KEY=VALUE");

                uint32_t key = read_number(value.substr(0, equal));
                if (!key_ids.insert(key).second)
                    throw std::runtime_error("Repeated graph key");

                keys.emplace_back(key, read_number(value.substr(equal + 1)));
            } else if (option == "--exports")
                exports = value;
            else if (option == "--input")
                input = value;
            else if (option == "--output")
                output = value;
            else if (option == "--report")
                report = value;
            else if (option == "--graph-id")
                graph_id = read_number(value);
            else if (option == "--resource-manager")
                resources = value;
            else if (option == "--usecase-kv")
                usecases = value;
            else if (option == "--board-dtb")
                dtb = value;
            else if (option == "--device")
                devices.append(value);
            else if (option == "--selection-contexts")
                selection_contexts = value;
            else if (option == "--hardware-widgets")
                hardware_widgets = value;
            else if (option == "--pcm-bindings")
                pcm_bindings = value;
            else if (option == "--bindings-output")
                bindings_output = value;
            else if (option == "--stream-type")
                context["StreamType"] = value;
            else if (option == "--device-pp-type")
                context["DevicePPType"] = value;
            else if (option == "--context") {
                size_t separator = value.find('=');
                if (separator == std::string::npos || !separator ||
                    separator + 1 == value.size())
                    throw std::runtime_error("Context requires NAME=VALUE");

                context[value.substr(0, separator)] =
                    value.substr(separator + 1);
            }
            else
                throw std::runtime_error("Unknown option " + option);
        }

        if (report.empty() || int(!databases.empty()) + int(!exports.empty()) +
            int(!input.empty()) != 1 ||
            (databases.empty() != keys.empty()) ||
            (!exports.empty() && !output.empty() && !corpus))
            throw std::runtime_error("Invalid options; use --help");

        int board_inputs = int(!resources.empty()) + int(!usecases.empty()) +
                           int(!dtb.empty());
        if (board_inputs && board_inputs != 3)
            throw std::runtime_error("Supply all three board inputs");

        if (!hardware_widgets.empty() && (!board_inputs || !corpus))
            throw std::runtime_error(
                "Hardware widgets require board inputs and corpus mode");

        if (!pcm_bindings.empty() && hardware_widgets.empty())
            throw std::runtime_error("PCM bindings require hardware widgets");

        if (!bindings_output.empty() && pcm_bindings.empty())
            throw std::runtime_error("Bindings output requires PCM bindings");

        if (!board_inputs && (!context.empty() || !devices.empty() ||
                             !selection_contexts.empty()))
            throw std::runtime_error("Device context requires board inputs");

        if (!selection_contexts.empty() &&
            (!context.empty() || !devices.empty()))
            throw std::runtime_error(
                "Selection contexts cannot use global context");

        Json::Value board;

        if (board_inputs)
            board = acdb_topology::read_board(resources, usecases, dtb);

        if (!context.empty())
            board["context"] = context;

        if (!devices.empty())
            board["devices"] = devices;

        std::vector<Json::Value> graphs;

        if (!databases.empty())
            graphs.push_back(acdb_topology::query_graph(databases, keys));
        else if (!input.empty())
            graphs.push_back(read_json(input));
        else {
            std::vector<fs::path> paths;

            for (const auto &entry : fs::directory_iterator(exports)) {
                if (entry.is_regular_file() && entry.path().extension() ==
                    ".json")
                    paths.push_back(entry.path());
            }

            std::sort(paths.begin(), paths.end());

            for (const auto &path : paths)
                graphs.push_back(read_json(path));
        }

        if (graphs.empty())
            throw std::runtime_error("No graphs to compile");

        std::vector<acdb_topology::Source> sources;

        for (const auto &graph : graphs)
            sources.push_back(acdb_topology::read_source(graph));

        Json::Value result;
        result["scope"] = "graph declarations";
        result["dsp_execution_verified"] = false;
        result["graphs"] = Json::Value(Json::arrayValue);
        Json::Value counts(Json::objectValue);
        size_t supported = 0;
        std::string topology;

        result["factored"] = acdb_topology::factor_sources(sources);
        acdb_topology::resolve_connections(result["factored"]);
        if (!selection_contexts.empty()) {
            acdb_topology::qualify_selections(result["factored"],
                                              read_json(selection_contexts));
            result["selection_contexts"] = selection_contexts;
        }

        acdb_topology::describe_endpoints(result["factored"], board);

        for (size_t i = 0; i < sources.size(); i++) {
            const auto &endpoints = result["factored"]["selections"]
                                          [Json::ArrayIndex(i)]["endpoints"];
            auto compiled = acdb_topology::compile_graph(sources[i], graph_id,
                                                          endpoints);
            if (compiled.report["can_emit"].asBool())
                supported++;

            for (const auto &issue : compiled.report["issues"]) {
                std::string kind = issue["kind"].asString();
                counts[kind] = counts.get(kind, 0).asUInt64() + 1;
            }

            result["graphs"].append(compiled.report);
            topology = compiled.topology.render();
        }

        result["issues"] = counts;
        result["can_emit"] = Json::UInt64(supported);
        result["selections"] = Json::UInt64(graphs.size());
        result["board"] = board;

        bool can_emit = supported == graphs.size();

        if (corpus) {
            Json::Value widgets;

            if (!hardware_widgets.empty()) {
                widgets = acdb_topology::read_hardware_widgets(
                    read_json(hardware_widgets), board);
                result["hardware_widgets"] = hardware_widgets;
            }

            auto compiled = acdb_topology::compile_corpus(
                sources, result["factored"], graph_id, widgets);
            if (!pcm_bindings.empty()) {
                acdb_topology::bind_frontends(compiled,
                                              read_json(pcm_bindings));
                result["pcm_bindings"] = pcm_bindings;
            }

            result["corpus"] = compiled.report;
            can_emit = compiled.report["can_emit"].asBool();
            topology = compiled.topology.render();
        }

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        write(report, Json::writeString(writer, result) + "\n");

        if (!can_emit) {
            std::cerr << "Unsupported graph declarations; see " << report
                      << ". No topology was written.\n";
            return 2;
        }

        if (!output.empty())
            write(output, topology);

        if (!bindings_output.empty()) {
            Json::Value bindings;
            bindings["version"] = 1;
            bindings["dsp_execution_verified"] = false;
            bindings["frontends"] = result["corpus"]["frontends"];
            bindings["selections"] = result["corpus"]["selections"];
            bindings["references"] = Json::Value(Json::arrayValue);
            bindings["hardware_gates"] = Json::Value(Json::arrayValue);

            for (const auto &subgraph : result["corpus"]["subgraphs"]) {
                for (const auto &gate : subgraph["hardware_gates"])
                    bindings["hardware_gates"].append(gate);
            }

            for (const auto &reference : result["corpus"]["references"]) {
                Json::Value value;

                for (const char *field : {"name", "control", "source_graph",
                                          "destination_graph", "path_control"}) {
                    if (reference.isMember(field))
                        value[field] = reference[field];
                }

                bindings["references"].append(value);
            }

            for (Json::ArrayIndex i = 0; i < bindings["selections"].size(); i++)
                bindings["selections"][i]["graph_keys"] =
                    result["factored"]["selections"][i]["metadata"]["graph_keys"];

            write(bindings_output, Json::writeString(writer, bindings) + "\n");
        }

        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
