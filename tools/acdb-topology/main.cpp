/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

uint32_t number(const std::string &text)
{
    size_t end;

    if (text.empty() || text[0] < '0' || text[0] > '9')
        throw std::runtime_error("Invalid unsigned integer");

    unsigned long long value = std::stoull(text, &end, 0);
    if (end != text.size() || value > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Unsigned integer is out of range");

    return value;
}

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
        uint32_t graph_id = 0;

        for (int i = 1; i < argc; i++) {
            std::string option = argv[i];
            if (option == "--help") {
                std::cout << "Usage: ar-acdb-topology --report FILE\n"
                          << "  (--acdb FILE ... --key ID=VALUE ... |\n"
                          << "   --input EXPORT.json | --exports DIRECTORY)\n"
                          << "  [--graph-id ID] [--output TOPOLOGY.conf]\n";
                return 0;
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

                uint32_t key = number(value.substr(0, equal));
                if (!key_ids.insert(key).second)
                    throw std::runtime_error("Repeated graph key");

                keys.emplace_back(key, number(value.substr(equal + 1)));
            } else if (option == "--exports")
                exports = value;
            else if (option == "--input")
                input = value;
            else if (option == "--output")
                output = value;
            else if (option == "--report")
                report = value;
            else if (option == "--graph-id")
                graph_id = number(value);
            else
                throw std::runtime_error("Unknown option " + option);
        }

        if (report.empty() || int(!databases.empty()) + int(!exports.empty()) +
            int(!input.empty()) != 1 ||
            (databases.empty() != keys.empty()) ||
            (!exports.empty() && !output.empty()))
            throw std::runtime_error("Invalid options; use --help");

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

        Json::Value result;
        result["scope"] = "graph declarations";
        result["dsp_execution_verified"] = false;
        result["graphs"] = Json::Value(Json::arrayValue);
        Json::Value counts(Json::objectValue);
        size_t supported = 0;
        std::string topology;

        for (const auto &graph : graphs) {
            auto compiled = acdb_topology::compile_graph(graph, graph_id);
            if (compiled.report["can_emit"].asBool())
                supported++;

            for (const auto &issue : compiled.report["issues"]) {
                std::string kind = issue["kind"].asString();
                counts[kind] = counts.get(kind, 0).asUInt64() + 1;
            }

            result["graphs"].append(compiled.report);
            topology = std::move(compiled.topology);
        }

        result["issues"] = counts;
        result["can_emit"] = Json::UInt64(supported);
        result["selections"] = Json::UInt64(graphs.size());
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        write(report, Json::writeString(writer, result) + "\n");

        if (supported != graphs.size()) {
            std::cerr << "Unsupported graph declarations; see " << report
                      << ". No topology was written.\n";
            return 2;
        }

        if (!output.empty())
            write(output, topology);

        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
