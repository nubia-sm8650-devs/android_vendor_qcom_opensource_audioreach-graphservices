/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <map>

namespace acdb_topology
{
namespace
{

Json::Value properties(const std::vector<Property> &source)
{
    Json::Value result(Json::arrayValue);

    for (const auto &property : source) {
        Json::Value value;
        value["id"] = property.id;
        value["payload"] = hex(property.payload);
        result.append(value);
    }

    return result;
}

Json::Value declaration(const Declaration &source)
{
    Json::Value result;
    result["id"] = source.id;
    result["properties"] = properties(source.properties);

    return result;
}

struct Record {
    const Parameter *parameter;
    std::string kind;
    std::vector<Bytes> references;
};

struct Selection {
    const Source *source;
    std::vector<Record> open, connections;
    std::vector<Bytes> driver, adjacency;
};

struct Definitions {
    std::map<std::string, std::map<Bytes, Json::Value>> values;
    std::map<std::string, std::map<Bytes, uint32_t>> indices;

    void add(const std::string &kind, const Bytes &bytes,
             const Json::Value &value)
    {
        values[kind].emplace(bytes, value);
    }

    std::vector<Record> records(const std::vector<Parameter> &parameters)
    {
        std::vector<Record> result;

        for (const auto &parameter : parameters) {
            Record record{&parameter, {}, {}};
            Reader reader{parameter.payload};

            if (parameter.target == 1) {
                switch (parameter.id) {
                case 0x08001000:
                case 0x08001001:
                case 0x08001003:
                    record.kind = parameter.id == 0x08001000 ? "containers" :
                                  parameter.id == 0x08001001 ? "subgraphs" :
                                  "module_properties";

                    for (const auto &item : read_declarations(reader)) {
                        add(record.kind, item.bytes, declaration(item));
                        record.references.push_back(item.bytes);
                    }
                    break;
                case 0x08001002:
                    record.kind = "module_groups";

                    for (const auto &item : read_module_groups(reader)) {
                        Json::Value value;
                        value["subgraph"] = item.subgraph;
                        value["container"] = item.container;
                        value["modules"] = Json::Value(Json::arrayValue);

                        for (const auto &module : item.modules) {
                            Json::Value identity;
                            identity["id"] = module.instance;
                            identity["type"] = module.type;
                            value["modules"].append(identity);
                        }

                        add(record.kind, item.bytes, value);
                        record.references.push_back(item.bytes);
                    }
                    break;
                case 0x08001004:
                case 0x08001061: {
                    bool control = parameter.id == 0x08001061;
                    record.kind = control ? "control_links" : "data_links";

                    for (const auto &item : read_connections(reader,
                                                              control)) {
                        Json::Value value;
                        value["peers"] = Json::Value(Json::arrayValue);

                        for (uint32_t peer : item.peers)
                            value["peers"].append(peer);

                        if (control)
                            value["properties"] = properties(item.properties);

                        add(record.kind, item.bytes, value);
                        record.references.push_back(item.bytes);
                    }
                    break;
                }
                default:
                    break;
                }
            }

            if (!record.kind.empty())
                reader.finish();

            result.push_back(std::move(record));
        }

        return result;
    }

    Json::Value references(const std::string &kind,
                            const std::vector<Bytes> &source) const
    {
        Json::Value result(Json::arrayValue);

        for (const auto &bytes : source)
            result.append(indices.at(kind).at(bytes));

        return result;
    }

    Json::Value record_list(const std::vector<Record> &records) const
    {
        Json::Value result(Json::arrayValue);

        for (const auto &record : records) {
            const auto &parameter = *record.parameter;
            Json::Value value;
            value["target"] = parameter.target;
            value["parameter"] = parameter.id;
            value["error"] = parameter.error;
            value["padding"] = hex(parameter.padding);

            if (record.kind.empty())
                value["payload"] = hex(parameter.payload);
            else {
                value["kind"] = record.kind;
                value["items"] = references(record.kind, record.references);
            }

            result.append(value);
        }

        return result;
    }
};

}

Json::Value factor_sources(const std::vector<Source> &sources)
{
    Definitions definitions;
    std::vector<Selection> selections;

    for (const auto &source : sources) {
        Selection selection;
        selection.source = &source;
        selection.open = definitions.records(source.open);
        selection.connections = definitions.records(source.connections);
        Reader driver{unhex(source.original["driver_properties"])};

        while (driver.remaining()) {
            auto item = read_declaration(driver);
            definitions.add("driver_properties", item.bytes,
                            declaration(item));
            selection.driver.push_back(item.bytes);
        }

        Reader adjacency{unhex(source.original["subgraph_connections"])};

        while (adjacency.remaining()) {
            size_t start = adjacency.position;
            uint32_t id = adjacency.word(), count = adjacency.count(4);
            Json::Value value;
            value["id"] = id;
            value["peers"] = Json::Value(Json::arrayValue);

            for (uint32_t i = 0; i < count; i++)
                value["peers"].append(adjacency.word());

            Bytes bytes(adjacency.bytes.begin() + start,
                        adjacency.bytes.begin() + adjacency.position);
            definitions.add("adjacency", bytes, value);
            selection.adjacency.push_back(bytes);
        }

        selections.push_back(std::move(selection));
    }

    Json::Value result;
    result["definitions"] = Json::Value(Json::objectValue);
    result["selections"] = Json::Value(Json::arrayValue);

    for (const auto &[kind, entries] : definitions.values) {
        auto &values = result["definitions"][kind];
        values = Json::Value(Json::arrayValue);

        for (const auto &[bytes, value] : entries) {
            definitions.indices[kind].emplace(bytes, values.size());
            values.append(value);
        }
    }

    for (const auto &selection : selections) {
        Json::Value value;
        value["metadata"] = selection.source->original;

        for (const char *name : {"graph_open", "graph_connections",
                                "driver_properties", "subgraph_connections"})
            value["metadata"].removeMember(name);

        value["graph_open"] = definitions.record_list(selection.open);
        value["graph_connections"] =
            definitions.record_list(selection.connections);
        value["driver_properties"] =
            definitions.references("driver_properties", selection.driver);
        value["subgraph_connections"] =
            definitions.references("adjacency", selection.adjacency);
        result["selections"].append(value);
    }

    return result;
}

}
