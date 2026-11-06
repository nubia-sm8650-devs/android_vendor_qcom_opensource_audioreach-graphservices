/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef ACDB_TOPOLOGY_SOURCE_H
#define ACDB_TOPOLOGY_SOURCE_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <json/json.h>

namespace acdb_topology
{

using Bytes = std::vector<uint8_t>;

struct Reader {
    Bytes bytes;
    size_t position = 0;

    size_t remaining() const
    {
        return bytes.size() - position;
    }

    Bytes take(size_t count);
    uint32_t word();
    uint32_t count(size_t stride);
    void finish() const;
};

struct Property {
    uint32_t id;
    Bytes payload;
};

struct Declaration {
    uint32_t id;
    std::vector<Property> properties;
    Bytes bytes;
};

struct Module {
    uint32_t subgraph;
    uint32_t container;
    uint32_t type;
    uint32_t instance;
};

struct ModuleGroup {
    uint32_t subgraph;
    uint32_t container;
    std::vector<Module> modules;
    Bytes bytes;
};

struct Connection {
    std::array<uint32_t, 4> peers;
    std::vector<Property> properties;
    Bytes bytes;
};

struct Parameter {
    uint32_t target;
    uint32_t id;
    uint32_t error;
    Bytes payload;
    Bytes padding;
};

struct Source {
    Json::Value original;
    std::vector<Parameter> open, connections;
};

Bytes unhex(const Json::Value &value);
std::string hex(const Bytes &bytes);
std::vector<Property> read_properties(Reader &reader);
Declaration read_declaration(Reader &reader);
std::vector<Declaration> read_declarations(Reader &reader);
std::vector<ModuleGroup> read_module_groups(Reader &reader);
std::vector<Connection> read_connections(Reader &reader, bool control);
Source read_source(const Json::Value &source);

}

#endif
