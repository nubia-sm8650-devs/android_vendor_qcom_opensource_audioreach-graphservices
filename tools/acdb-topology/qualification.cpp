/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <algorithm>
#include <map>
#include <set>
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

Json::Value read_unavailable(const Json::Value &input, const Json::Value &board)
{
    fields(input, {"version", "devices", "selections"});

    if (!input["version"].isUInt() || input["version"].asUInt() != 1 ||
        !input["devices"].isArray() || !input["selections"].isArray())
        throw std::runtime_error("Invalid unavailable path version or lists");

    std::set<std::string> known;

    for (const auto &profile : board["profiles"])
        known.insert(profile["device"].asString());

    for (const auto &assignment : board["assignments"]) {
        for (const auto &device : assignment["devices"])
            known.insert(device.asString());
    }

    Json::Value result;
    result["devices"] = Json::Value(Json::objectValue);
    result["selections"] = Json::Value(Json::arrayValue);

    for (const auto &entry : input["devices"]) {
        fields(entry, {"device", "reason"});

        const auto &device = entry["device"];
        const auto &reason = entry["reason"];
        if (!device.isString() || !known.count(device.asString()) ||
            !reason.isString() || reason.asString().empty() ||
            result["devices"].isMember(device.asString()))
            throw std::runtime_error("Invalid or repeated unavailable device");

        result["devices"][device.asString()] = reason;
    }

    std::set<Keys> listed;

    for (const auto &entry : input["selections"]) {
        fields(entry, {"graph_keys", "reason"});

        const auto &reason = entry["reason"];
        if (!reason.isString() || reason.asString().empty() ||
            !listed.insert(selection_keys(entry["graph_keys"])).second)
            throw std::runtime_error(
                "Invalid or repeated unavailable selection");

        result["selections"].append(entry);
    }

    return result;
}

Json::Value excluded_selections(const Json::Value &factored,
                                const Json::Value &unavailable)
{
    std::map<Keys, std::string> listed;
    std::set<Keys> matched;

    for (const auto &entry : unavailable["selections"])
        listed.emplace(selection_keys(entry["graph_keys"]),
                       entry["reason"].asString());

    Json::Value result(Json::arrayValue);
    const auto &selections = factored["selections"];

    for (Json::ArrayIndex index = 0; index < selections.size(); index++) {
        const auto &metadata = selections[index]["metadata"];
        auto keys = selection_keys(metadata["graph_keys"]);
        Json::Value devices(Json::arrayValue);

        for (const auto &endpoint :
             selections[index]["endpoints"]["physical"]) {
            const auto &binding = endpoint["binding"];
            if (binding["status"] != "unavailable-device")
                continue;

            for (const auto &device : binding["unavailable_devices"])
                devices.append(device);
        }

        auto found = listed.find(keys);
        if (found == listed.end() && devices.empty())
            continue;

        Json::Value entry;
        entry["selection"] = index;
        entry["graph_keys"] = metadata["graph_keys"];

        if (found != listed.end()) {
            entry["reason"] = found->second;
            matched.insert(keys);
        }

        if (!devices.empty())
            entry["unavailable_devices"] = devices;

        result.append(entry);
    }

    if (matched.size() != listed.size())
        throw std::runtime_error("Unavailable selection matches no source");

    return result;
}

Json::Value remaining_contexts(const Json::Value &contexts,
                               const Json::Value &excluded)
{
    std::set<Keys> removed;

    for (const auto &entry : excluded)
        removed.insert(selection_keys(entry["graph_keys"]));

    Json::Value result = contexts;
    result["selections"] = Json::Value(Json::arrayValue);

    for (const auto &entry : contexts["selections"]) {
        if (!removed.count(selection_keys(entry["graph_keys"])))
            result["selections"].append(entry);
    }

    return result;
}

}
