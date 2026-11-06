/* SPDX-License-Identifier: BSD-3-Clause */
#include "source.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace acdb_topology
{

uint32_t read_number(const std::string &text)
{
    size_t end;

    if (text.empty() || text[0] < '0' || text[0] > '9')
        throw std::runtime_error("Invalid unsigned integer");

    unsigned long long value = std::stoull(text, &end, 0);
    if (end != text.size() || value > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Unsigned integer is out of range");

    return value;
}

Bytes Reader::take(size_t count)
{
    if (count > remaining())
        throw std::runtime_error("Truncated graph record");

    Bytes value(bytes.begin() + position, bytes.begin() + position + count);
    position += count;

    return value;
}

uint32_t Reader::word()
{
    Bytes value = take(4);

    return uint32_t(value[0]) | uint32_t(value[1]) << 8 |
           uint32_t(value[2]) << 16 | uint32_t(value[3]) << 24;
}

uint32_t Reader::count(size_t stride)
{
    uint32_t value = word();
    if (value > remaining() / stride)
        throw std::runtime_error("Invalid graph element count");

    return value;
}

void Reader::finish() const
{
    if (remaining())
        throw std::runtime_error("Trailing graph data");
}

Bytes unhex(const Json::Value &value)
{
    if (!value.isString())
        throw std::runtime_error("Graph blob must be hexadecimal text");

    std::string text = value.asString();
    Bytes result;

    if (text.size() % 2)
        throw std::runtime_error("Odd hexadecimal blob length");

    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return c - '0';

        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;

        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;

        throw std::runtime_error("Invalid hexadecimal blob");
    };

    for (size_t i = 0; i < text.size(); i += 2)
        result.push_back(digit(text[i]) * 16 + digit(text[i + 1]));

    return result;
}

std::string hex(const Bytes &bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;

    for (uint8_t value : bytes) {
        result += digits[value >> 4];
        result += digits[value & 15];
    }

    return result;
}

std::vector<Property> read_properties(Reader &reader)
{
    uint32_t count = reader.count(8);
    std::vector<Property> result;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = reader.word();
        result.push_back({id, reader.take(reader.word())});
    }

    return result;
}

Declaration read_declaration(Reader &reader)
{
    size_t start = reader.position;
    uint32_t id = reader.word();
    auto properties = read_properties(reader);
    Bytes bytes(reader.bytes.begin() + start,
                reader.bytes.begin() + reader.position);

    return {id, std::move(properties), std::move(bytes)};
}

std::vector<Declaration> read_declarations(Reader &reader)
{
    uint32_t count = reader.count(8);
    std::vector<Declaration> result;

    for (uint32_t i = 0; i < count; i++)
        result.push_back(read_declaration(reader));

    return result;
}

std::vector<ModuleGroup> read_module_groups(Reader &reader)
{
    uint32_t count = reader.count(12);
    std::vector<ModuleGroup> result;

    for (uint32_t i = 0; i < count; i++) {
        size_t start = reader.position;
        ModuleGroup group;
        group.subgraph = reader.word();
        group.container = reader.word();
        uint32_t modules = reader.count(8);

        for (uint32_t j = 0; j < modules; j++) {
            uint32_t type = reader.word(), id = reader.word();
            group.modules.push_back({group.subgraph, group.container,
                                     type, id});
        }

        group.bytes = Bytes(reader.bytes.begin() + start,
                            reader.bytes.begin() + reader.position);
        result.push_back(std::move(group));
    }

    return result;
}

std::vector<Connection> read_connections(Reader &reader, bool control)
{
    uint32_t count = reader.count(control ? 20 : 16);
    std::vector<Connection> result;

    for (uint32_t i = 0; i < count; i++) {
        size_t start = reader.position;
        Connection connection;

        for (auto &value : connection.peers)
            value = reader.word();

        if (control)
            connection.properties = read_properties(reader);

        connection.bytes = Bytes(reader.bytes.begin() + start,
                                 reader.bytes.begin() + reader.position);
        result.push_back(std::move(connection));
    }

    return result;
}

std::vector<Parameter> read_parameters(const Json::Value &value)
{
    Reader records{unhex(value)};
    std::vector<Parameter> result;

    while (records.remaining()) {
        Parameter parameter;
        parameter.target = records.word();
        parameter.id = records.word();
        uint32_t size = records.word();
        parameter.error = records.word();
        parameter.payload = records.take(size);
        parameter.padding = records.take((8 - size % 8) % 8);

        if (parameter.error)
            throw std::runtime_error("Nonzero graph parameter error");

        result.push_back(std::move(parameter));
    }

    return result;
}

Source read_source(const Json::Value &source)
{
    return {source, read_parameters(source["graph_open"]),
            read_parameters(source["graph_connections"])};
}

}
