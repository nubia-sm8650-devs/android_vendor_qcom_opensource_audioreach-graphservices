/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace acdb_topology
{
namespace
{

Keys selection_keys(const Json::Value &values)
{
    if (!values.isArray())
        throw std::runtime_error("Selection context requires graph_keys");

    Keys result;
    std::set<uint32_t> ids;

    for (const auto &pair : values) {
        if (!pair.isArray() || pair.size() != 2 || !pair[0].isString() ||
            !pair[1].isString())
            throw std::runtime_error("Invalid selection context graph key");

        auto key = read_number(pair[0].asString());
        if (!ids.insert(key).second)
            throw std::runtime_error("Repeated selection context graph key");

        result.emplace_back(key, read_number(pair[1].asString()));
    }

    std::sort(result.begin(), result.end());

    return result;
}

void fields(const Json::Value &value, const std::set<std::string> &allowed)
{
    if (!value.isObject())
        throw std::runtime_error("Selection context must be an object");

    for (const auto &name : value.getMemberNames()) {
        if (!allowed.count(name))
            throw std::runtime_error("Unknown selection context field " + name);
    }
}

Json::Value configuration(const Json::Value &value)
{
    fields(value, {"graph_keys", "devices", "context"});

    if (!value["devices"].isArray() || value["devices"].empty() ||
        !value["context"].isObject())
        throw std::runtime_error(
            "Selection context requires devices and context");

    std::set<std::string> devices;

    for (const auto &device : value["devices"]) {
        if (!device.isString() || device.asString().empty() ||
            !devices.insert(device.asString()).second)
            throw std::runtime_error("Invalid selection context device");
    }

    for (const auto &name : value["context"].getMemberNames()) {
        const auto &item = value["context"][name];
        bool valid = item.isNull() ||
                     (item.isString() && !item.asString().empty());
        if (name.empty() || !valid)
            throw std::runtime_error("Invalid selection context selector");
    }

    Json::Value result;
    result["devices"] = value["devices"];
    result["context"] = value["context"];

    return result;
}

}

void qualify_selections(Json::Value &factored, const Json::Value &contexts)
{
    fields(contexts, {"version", "selections"});

    if (!contexts["version"].isUInt() || contexts["version"].asUInt() != 1 ||
        !contexts["selections"].isArray())
        throw std::runtime_error(
            "Invalid selection context version or selections");

    std::map<Keys, Json::Value> bindings;
    std::set<Keys> selected;

    for (const auto &selection : factored["selections"])
        selected.insert(selection_keys(selection["metadata"]["graph_keys"]));

    for (const auto &value : contexts["selections"]) {
        auto keys = selection_keys(value["graph_keys"]);
        if (!selected.count(keys))
            throw std::runtime_error("Selection context matches no source");

        if (!bindings.emplace(keys, configuration(value)).second)
            throw std::runtime_error("Repeated selection context");
    }

    for (auto &selection : factored["selections"]) {
        auto binding = bindings.find(
            selection_keys(selection["metadata"]["graph_keys"]));
        if (binding != bindings.end())
            selection["hardware_context"] = binding->second;
    }
}

}
