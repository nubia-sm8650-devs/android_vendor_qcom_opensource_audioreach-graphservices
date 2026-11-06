/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <acdb.h>
#include <acdb_types.h>
#include <ar_osal_error.h>

namespace acdb_topology
{
namespace
{

void query(uint32_t command, void *request, uint32_t request_size,
           void *response, uint32_t response_size)
{
    int status = acdb_ioctl(command, request, request_size,
                            response, response_size);
    if (status)
        throw std::runtime_error("ACDB command " + std::to_string(command) +
                                 " failed with " + std::to_string(status));
}

struct Database {
    explicit Database(const std::vector<std::string> &paths)
    {
        AcdbDataFiles files{};

        if (paths.empty() || paths.size() > MAX_ACDB_FILE_COUNT)
            throw std::runtime_error("Invalid database file count");

        files.num_files = paths.size();

        for (size_t i = 0; i < paths.size(); i++) {
            auto &file = files.acdbFiles[i];
            if (paths[i].size() >= sizeof(file.fileName))
                throw std::runtime_error("Database path is too long");

            std::memcpy(file.fileName, paths[i].c_str(), paths[i].size() + 1);
            file.fileNameLen = paths[i].size();
        }

        int status = acdb_init(&files, nullptr);
        if (status) {
            acdb_deinit();
            throw std::runtime_error("ACDB initialization failed with " +
                                     std::to_string(status));
        }
    }

    ~Database()
    {
        acdb_deinit();
    }
};

std::vector<uint8_t> storage(uint32_t size)
{
    return std::vector<uint8_t>(size ? size : 1);
}

void check_size(uint32_t size, size_t capacity)
{
    if (size > capacity)
        throw std::runtime_error("ACDB response exceeds its allocation");
}

}

Json::Value query_graph(const std::vector<std::string> &paths,
                       const Keys &keys)
{
    if (keys.empty() || keys.size() > ACDB_MAX_KEY_COUNT)
        throw std::runtime_error("Invalid graph key count");

    std::vector<AcdbKeyValuePair> native_keys;

    for (auto [key, value] : keys)
        native_keys.push_back({key, value});

    Database database(paths);
    AcdbGraphKeyVector key_vector{uint32_t(keys.size()), native_keys.data()};
    AcdbGetGraphRsp graph{};
    query(ACDB_CMD_GET_GRAPH, &key_vector, sizeof(key_vector),
          &graph, sizeof(graph));
    auto adjacency = storage(graph.size);
    graph.subgraphs = reinterpret_cast<AcdbSubgraph *>(adjacency.data());
    query(ACDB_CMD_GET_GRAPH, &key_vector, sizeof(key_vector),
          &graph, sizeof(graph));
    check_size(graph.size, adjacency.size());
    adjacency.resize(graph.size);

    std::vector<uint32_t> ids;
    size_t position = 0;

    auto word = [&]() {
        if (position > adjacency.size() || adjacency.size() - position < 4)
            throw std::runtime_error("Truncated ACDB adjacency");

        uint32_t value;
        std::memcpy(&value, adjacency.data() + position, 4);
        position += 4;

        return value;
    };

    for (uint32_t i = 0; i < graph.num_subgraphs; i++) {
        ids.push_back(word());
        uint32_t count = word();
        if (count > (adjacency.size() - position) / 4)
            throw std::runtime_error("Invalid ACDB adjacency count");

        position += count * 4;
    }

    if (position != adjacency.size())
        throw std::runtime_error("Trailing ACDB adjacency");

    AcdbSgIdGraphKeyVector request{};
    request.graph_key_vector = key_vector;
    request.num_sgid = ids.size();
    request.sg_ids = ids.data();
    AcdbGetSubgraphDataRsp data{};
    std::vector<uint8_t> properties, driver, connections;

    if (!ids.empty()) {
        query(ACDB_CMD_GET_SUBGRAPH_DATA, &request, sizeof(request),
              &data, sizeof(data));
        driver = storage(data.driver_prop.size);
        properties = storage(data.spf_blob.buf_size);
        data.driver_prop.sub_graph_prop_data =
            reinterpret_cast<AcdbSubGraphPropertyData *>(driver.data());
        data.spf_blob.buf = properties.data();
        query(ACDB_CMD_GET_SUBGRAPH_DATA, &request, sizeof(request),
              &data, sizeof(data));
        check_size(data.driver_prop.size, driver.size());
        check_size(data.spf_blob.buf_size, properties.size());
        driver.resize(data.driver_prop.size);
        properties.resize(data.spf_blob.buf_size);

        AcdbSubGraphList peers{graph.num_subgraphs, graph.subgraphs};
        AcdbBlob links{};
        int status = acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CONNECTIONS,
                                &peers, sizeof(peers), &links, sizeof(links));
        if (status && status != AR_ENOTEXIST)
            throw std::runtime_error("ACDB connection query failed with " +
                                     std::to_string(status));

        if (!status && links.buf_size) {
            connections = storage(links.buf_size);
            links.buf = connections.data();
            query(ACDB_CMD_GET_SUBGRAPH_CONNECTIONS, &peers, sizeof(peers),
                  &links, sizeof(links));
            check_size(links.buf_size, connections.size());
            connections.resize(links.buf_size);
        }
    }

    auto text = [](uint32_t value) {
        char buffer[11];
        std::snprintf(buffer, sizeof(buffer), "0x%08x", value);

        return std::string(buffer);
    };

    Json::Value result;
    result["version"] = 1;
    result["graph_keys"] = Json::Value(Json::arrayValue);
    result["subgraph_ids"] = Json::Value(Json::arrayValue);

    for (auto [key, value] : keys) {
        Json::Value pair(Json::arrayValue);
        pair.append(text(key));
        pair.append(text(value));
        result["graph_keys"].append(pair);
    }

    for (uint32_t id : ids)
        result["subgraph_ids"].append(text(id));

    result["driver_subgraph_count"] = data.driver_prop.num_sgid;
    result["subgraph_connections"] = hex(adjacency);
    result["driver_properties"] = hex(driver);
    result["graph_open"] = hex(properties);
    result["graph_connections"] = hex(connections);

    return result;
}

}
