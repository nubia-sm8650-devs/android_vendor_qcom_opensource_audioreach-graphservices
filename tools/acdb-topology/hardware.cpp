/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <array>
#include <set>
#include <sstream>
#include <sound/snd_ar_tokens.h>

namespace acdb_topology
{
namespace
{

bool contains(const std::string &list, const std::string &value)
{
    std::istringstream stream(list);
    std::string item;

    while (std::getline(stream, item, ',')) {
        size_t start = item.find_first_not_of(" \t\r\n");
        size_t end = item.find_last_not_of(" \t\r\n");
        if (start != std::string::npos &&
            item.substr(start, end - start + 1) == value)
            return true;
    }

    return false;
}

Json::Value bind(const Json::Value &endpoint, const Json::Value &board)
{
    Json::Value result;
    result["status"] = "unsupported-interface";

    std::string family = endpoint["family"].asString();
    if (family != "MI2S" && family != "CODEC_DMA" && family != "SLIM")
        return result;

    std::set<std::array<uint32_t, 3>> ports;
    Json::Value sources(Json::arrayValue);
    bool unresolved = false;

    for (Json::ArrayIndex i = 0; i < endpoint["board_candidates"].size(); i++) {
        const auto &candidate = endpoint["board_candidates"][i];
        const auto &assignment = board["assignments"]
                                      [candidate["assignment"].asUInt()];
        const auto &profile = candidate["profile"].isNull() ? Json::Value() :
            board["profiles"][candidate["profile"].asUInt()];
        const auto &device = profile.isNull() ? candidate["device"] :
                                                profile["device"];
        bool selected = board["devices"].empty();

        for (const auto &id : board["devices"])
            selected |= id == device;

        if (!selected)
            continue;

        bool matches = true, missing = false;
        const auto &conditions = assignment["conditions"];

        for (const auto &name : conditions.getMemberNames()) {
            if (!board["context"].isMember(name))
                missing = true;
            else if (!contains(conditions[name].asString(),
                               board["context"][name].asString()))
                matches = false;
        }

        if (!matches)
            continue;

        if (missing || candidate["status"] != "candidate") {
            unresolved = true;
            continue;
        }

        for (const auto &index : candidate["links"]) {
            const auto &link = board["links"][index.asUInt()];
            const auto &transport = link["transport"];
            if (family == "SLIM")
                ports.insert({0, profile["transport"]["device"].asUInt(),
                              link["dai"].asUInt()});
            else
                ports.insert({transport["bank"].asUInt(),
                              transport["index"].asUInt(),
                              link["dai"].asUInt()});
        }

        sources.append(i);
    }

    result["status"] = unresolved ? "unresolved-board-candidate" :
        ports.empty() ? "missing-board-binding" : "ambiguous-board-binding";

    if (unresolved || ports.size() != 1)
        return result;

    std::set<uint32_t> formats;

    for (const auto &parameter : endpoint["default_parameters"]) {
        if (parameter["parameter"].asUInt() != 0x08001017)
            continue;

        Reader data{unhex(parameter["payload"])};
        data.word();
        data.word();
        formats.insert(data.word());
        data.finish();
    }

    result["status"] = formats.empty() ? "missing-native-format" :
                                         "ambiguous-native-format";

    bool live_format = family == "SLIM" && formats.empty();
    if (!live_format && formats.size() != 1)
        return result;

    Json::Value tokens(Json::arrayValue);
    auto token = [&](uint32_t id, uint32_t value) {
        Json::Value pair(Json::arrayValue);
        pair.append(id);
        pair.append(value);
        tokens.append(pair);
    };
    const auto &port = *ports.begin();
    if (family != "SLIM")
        token(AR_TKN_U32_MODULE_HW_IF_TYPE, port[0]);

    token(AR_TKN_U32_MODULE_HW_IF_IDX, port[1]);
    if (!live_format)
        token(AR_TKN_U32_MODULE_FMT_DATA, *formats.begin());

    if (family == "MI2S") {
        std::set<std::array<uint32_t, 2>> interfaces;

        for (const auto &variant : endpoint["tag_parameters"]) {
            for (const auto &parameter : variant["parameters"]) {
                if (parameter["parameter"].asUInt() != 0x08001019)
                    continue;

                Reader data{unhex(parameter["payload"])};
                data.word();
                data.word();
                uint32_t lanes = data.word();
                data.finish();
                interfaces.insert({lanes & 0xffff, lanes >> 16});
            }
        }

        result["status"] = interfaces.empty() ? "missing-native-interface" :
                                               "ambiguous-native-interface";

        if (interfaces.size() != 1)
            return result;

        token(AR_TKN_U32_MODULE_SD_LINE_IDX, interfaces.begin()->at(0));
        token(AR_TKN_U32_MODULE_WS_SRC, interfaces.begin()->at(1));
    }

    result["status"] = live_format ? "dai-configured" : "static-bound";
    if (live_format)
        result["required_runtime_format"] = true;

    result["dai"] = port[2];
    result["tokens"] = tokens;
    result["candidates"] = sources;

    return result;
}

}

void bind_hardware(Json::Value &endpoints, const Json::Value &board)
{
    for (auto &endpoint : endpoints["physical"]) {
        unsigned count = 0;

        for (const auto &other : endpoints["physical"])
            count += other["direction"] == endpoint["direction"];

        if (count != 1)
            endpoint["binding"]["status"] = "multiple-physical-endpoints";
        else
            endpoint["binding"] = bind(endpoint, board);
    }
}

}
