/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <array>
#include <map>
#include <set>
#include <stdexcept>

namespace acdb_topology
{
namespace
{

struct Role {
    const char *direction;
    const char *kind;
};

const std::map<uint32_t, Role> hosts{
    {0x07001000, {"playback", "packet"}},
    {0x07001001, {"capture", "packet"}},
    {0x07001006, {"playback", "pull"}},
    {0x07001007, {"capture", "push"}},
};

const std::map<uint32_t, Role> physical{
    {0x0700100a, {"playback", "MI2S"}},
    {0x0700100b, {"capture", "MI2S"}},
    {0x07001023, {"playback", "CODEC_DMA"}},
    {0x07001024, {"capture", "CODEC_DMA"}},
    {0x07001029, {"playback", "SLIM"}},
    {0x0700102a, {"capture", "SLIM"}},
    {0x0700104f, {"playback", "USB_AUDIO"}},
    {0x07001050, {"capture", "USB_AUDIO"}},
    {0x07001069, {"playback", "DISPLAY_PORT"}},
};

Json::Value read_tags(const Json::Value &queries)
{
    Json::Value result(Json::arrayValue);
    Reader reader{unhex(queries["tags"]["data"])};

    for (uint32_t i = 0; i < queries["tags"]["count"].asUInt(); i++) {
        Json::Value tag;
        tag["tag"] = reader.word();
        tag["modules"] = Json::Value(Json::arrayValue);
        uint32_t count = reader.count(8);

        for (uint32_t j = 0; j < count; j++) {
            Json::Value module;
            module["type"] = reader.word();
            module["module"] = reader.word();
            tag["modules"].append(module);
        }

        result.append(tag);
    }

    reader.finish();

    return result;
}

Json::Value read_variants(const Json::Value &queries)
{
    Json::Value result(Json::arrayValue);
    Reader reader{unhex(queries["tag_keys"]["data"])};

    for (uint32_t i = 0; i < queries["tag_keys"]["count"].asUInt(); i++) {
        Json::Value variant;
        variant["tag"] = reader.word();
        variant["keys"] = Json::Value(Json::arrayValue);
        uint32_t count = reader.count(8);

        for (uint32_t j = 0; j < count; j++) {
            Json::Value pair(Json::arrayValue);
            pair.append(reader.word());
            pair.append(reader.word());
            variant["keys"].append(pair);
        }

        result.append(variant);
    }

    reader.finish();

    return result;
}

Json::Value parameters(const Json::Value &data, uint32_t module)
{
    Json::Value result(Json::arrayValue);

    for (const auto &parameter : read_parameters(data)) {
        if (parameter.target != module)
            continue;

        Json::Value value;
        value["parameter"] = parameter.id;
        value["payload"] = hex(parameter.payload);
        value["padding"] = hex(parameter.padding);
        result.append(value);
    }

    return result;
}

Json::Value board_candidates(const Json::Value &metadata,
                              const Json::Value &endpoint,
                              const Json::Value &board)
{
    std::set<std::pair<uint32_t, uint32_t>> keys;

    for (const auto &pair : metadata["graph_keys"])
        keys.emplace(read_number(pair[0].asString()),
                     read_number(pair[1].asString()));

    Json::Value result(Json::arrayValue);

    for (Json::ArrayIndex i = 0; i < board["assignments"].size(); i++) {
        const auto &assignment = board["assignments"][i];
        bool matches = true, device_key = false;

        for (const auto &pair : assignment["keys"]) {
            uint32_t key = pair[0].asUInt();
            matches &= keys.count({key, pair[1].asUInt()}) != 0;

            if (endpoint["direction"] == "playback")
                device_key |= key == 0xa2000000;
            else
                device_key |= key == 0xa3000000 || key == 0xd7000000;
        }

        if (!matches || !device_key)
            continue;

        for (const auto &device : assignment["devices"]) {
            bool profiled = false;

            for (Json::ArrayIndex p = 0; p < board["profiles"].size(); p++) {
                const auto &profile = board["profiles"][p];
                if (profile["device"] != device)
                    continue;

                profiled = true;

                Json::Value candidate;
                candidate["assignment"] = i;
                candidate["profile"] = p;
                candidate["links"] = profile["links"];
                candidate["binding"] = Json::Value();

                const auto &transport = profile["transport"];
                if (transport.isNull())
                    candidate["status"] = "unsupported-board-interface";
                else if (transport["family"] != endpoint["family"] ||
                         transport["direction"] != endpoint["direction"])
                    candidate["status"] = "interface-mismatch";
                else if (profile["links"].empty())
                    candidate["status"] = "missing-board-link";
                else
                    candidate["status"] = "candidate";

                result.append(candidate);
            }

            if (!profiled) {
                Json::Value candidate;
                candidate["assignment"] = i;
                candidate["device"] = device;
                candidate["profile"] = Json::Value();
                candidate["links"] = Json::Value(Json::arrayValue);
                candidate["binding"] = Json::Value();
                candidate["status"] = "missing-device-profile";
                result.append(candidate);
            }
        }
    }

    return result;
}

}

std::string module_widget_type(uint32_t module)
{
    auto host = hosts.find(module);
    if (host != hosts.end())
        return std::string(host->second.direction) == "playback" ?
               "aif_in" : "aif_out";

    if (physical.count(module))
        return "buffer";

    switch (module) {
    case 0x07001003: return "src";
    case 0x07001004: return "encoder";
    case 0x07001005:
    case 0x0700103b: return "decoder";
    case 0x0700101a:
    case 0x0700117c:
    case 0x0700117d: return "buffer";
    default: return "effect";
    }
}

void describe_endpoints(Json::Value &factored, const Json::Value &board)
{
    const auto &groups = factored["definitions"]["module_groups"];

    for (auto &selection : factored["selections"]) {
        Json::Value result;
        const auto &metadata = selection["metadata"];
        const auto &queries = metadata["endpoint_queries"];
        bool queried = !queries.isNull();
        result["native_queries_available"] = queried;
        result["runtime_bindings"] = false;
        result["tags"] = queried ? read_tags(queries) :
                                      Json::Value(Json::arrayValue);
        result["tag_variants"] = queried ? read_variants(queries) :
                                              Json::Value(Json::arrayValue);
        result["physical"] = Json::Value(Json::arrayValue);

        for (const char *direction : {"playback", "capture"})
            result["host"][direction]["candidates"] =
                Json::Value(Json::arrayValue);

        std::set<std::array<uint32_t, 4>> modules;

        for (const char *buffer : {"graph_open", "graph_connections"}) {
            for (const auto &record : selection[buffer]) {
                if (record.get("kind", "").asString() != "module_groups")
                    continue;

                for (const auto &reference : record["items"]) {
                    const auto &group = groups[reference.asUInt()];

                    for (const auto &module : group["modules"])
                        modules.insert({module["id"].asUInt(),
                            module["type"].asUInt(),
                            group["subgraph"].asUInt(),
                            group["container"].asUInt()});
                }
            }
        }

        for (const auto &[instance, type, subgraph, container] : modules) {
            Json::Value endpoint;
            endpoint["module"] = instance;
            endpoint["type"] = type;
            endpoint["subgraph"] = subgraph;
            endpoint["container"] = container;

            auto host = hosts.find(type);
            if (host != hosts.end()) {
                const auto &role = host->second;
                endpoint["direction"] = role.direction;
                endpoint["mode"] = role.kind;
                result["host"][role.direction]["candidates"].append(endpoint);
            }

            auto backend = physical.find(type);
            if (backend == physical.end())
                continue;

            endpoint["direction"] = backend->second.direction;
            endpoint["family"] = backend->second.kind;
            endpoint["default_parameters"] = queried ?
                parameters(queries["empty_ckv_defaults"]["data"], instance) :
                Json::Value(Json::arrayValue);
            endpoint["tag_parameters"] = Json::Value(Json::arrayValue);

            for (const auto &data : queries["hardware_tag_data"]) {
                uint32_t ref = data["variant"].asUInt();
                if (ref >= result["tag_variants"].size())
                    throw std::runtime_error("Invalid hardware tag variant");

                auto values = parameters(data["data"], instance);
                if (values.empty())
                    continue;

                Json::Value variant;
                variant["variant"] = ref;
                variant["parameters"] = values;
                endpoint["tag_parameters"].append(variant);
            }

            endpoint["board_candidates"] =
                board_candidates(metadata, endpoint, board);
            result["physical"].append(endpoint);
        }

        for (const char *direction : {"playback", "capture"}) {
            auto &host = result["host"][direction];
            size_t count = host["candidates"].size();
            host["resolution"] = count == 0 ? "none" : count == 1 ?
                                                        "unique" : "ambiguous";
        }

        selection["endpoints"] = std::move(result);
    }
}

}
