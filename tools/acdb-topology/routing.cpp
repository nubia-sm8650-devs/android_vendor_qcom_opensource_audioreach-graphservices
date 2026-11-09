/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <sstream>
#include <stdexcept>

namespace acdb_topology
{

Json::Value read_hardware_widgets(const Json::Value &input,
                                 const Json::Value &board)
{
    if (!input.isObject() || input["version"] != 1 ||
        !input["widgets"].isArray())
        throw std::runtime_error("Invalid hardware widget bindings");

    Json::Value result(Json::objectValue);
    std::set<std::string> names;
    std::set<uint32_t> aif_ids;
    std::set<std::string> aif_names;
    result["reserved_dais"] = Json::Value(Json::arrayValue);

    for (const auto &link : board["links"])
        result["reserved_dais"].append(link["dai"]);

    for (const auto &widget : input["widgets"]) {
        if (!widget["dai"].isUInt() || !widget["direction"].isString() ||
            !widget["name"].isString())
            throw std::runtime_error("Invalid hardware widget binding");

        std::string id = std::to_string(widget["dai"].asUInt());
        std::string direction = widget["direction"].asString();
        std::string name = widget["name"].asString();
        bool available = false;

        for (const auto &link : board["links"]) {
            if (link["dai"].asUInt() == widget["dai"].asUInt() &&
                link["transport"]["direction"] == direction)
                available = true;
        }

        if (!available ||
            (direction != "playback" && direction != "capture") ||
            name.empty() || name.size() >= 44 ||
            !names.insert(name).second ||
            result["widgets"][id].isMember(direction))
            throw std::runtime_error("Invalid or repeated hardware widget");

        for (unsigned char ch : name) {
            if (ch < 32 || ch >= 127 || ch == '"' || ch == '\\' || ch == ',')
                throw std::runtime_error("Invalid hardware widget name");
        }

        result["widgets"][id][direction] = name;
        if (widget.isMember("aif") && !widget.isMember("media_controls"))
            throw std::runtime_error("Native AIF lacks backend media controls");

        if (widget.isMember("media_controls")) {
            const auto &controls = widget["media_controls"];
            if (!controls.isObject())
                throw std::runtime_error("Invalid backend media controls");

            for (const char *key : {"rate", "format", "data_format"}) {
                if (!controls.isMember(key))
                    throw std::runtime_error("Missing backend media control");
            }

            for (const auto &key : controls.getMemberNames()) {
                const auto &value = controls[key];
                if ((key != "rate" && key != "format" && key != "channels" &&
                     key != "data_format" && key != "channel_map") ||
                    !value.isString() ||
                    value.asString().empty() || value.asString().size() >= 44)
                    throw std::runtime_error("Invalid backend media control");
            }

            if (controls.isMember("channels") && controls.isMember("channel_map"))
                throw std::runtime_error("Conflicting backend channel sources");

            Json::Value matches(Json::arrayValue);

            for (const auto &link : board["links"]) {
                if (link["dai"].asUInt() == widget["dai"].asUInt() &&
                    link["transport"]["direction"] == direction &&
                    (!widget.isMember("backend") ||
                     widget["backend"] == link["path"]))
                    matches.append(link);
            }

            if (matches.size() != 1)
                throw std::runtime_error("Ambiguous backend media identity");

            auto backend = matches[0];
            backend["controls"] = controls;
            if (widget.isMember("aif")) {
                const auto &aif = widget["aif"];
                if (!aif.isObject() || !aif["id"].isUInt() ||
                    !aif["name"].isString() ||
                    aif["name"].asString().empty() ||
                    aif["name"].asString().size() >= 32 ||
                    aif["name"].asString().find('\0') != std::string::npos ||
                    !aif_ids.insert(aif["id"].asUInt()).second ||
                    !aif_names.insert(aif["name"].asString()).second)
                    throw std::runtime_error("Invalid native AIF identity");

                for (const auto &key : aif.getMemberNames()) {
                    if (key != "id" && key != "name")
                        throw std::runtime_error("Unsupported native AIF field");
                }

                backend["aif"] = aif;
            }

            result["backends"][id][direction] = backend;
        }
    }

    return result;
}

void bind_hardware_widgets(Compilation &compiled, uint32_t graph_id,
                           const Json::Value &endpoints,
                           const Json::Value &widgets)
{
    if (!compiled.report["can_emit"].asBool())
        return;

    auto &gates = compiled.report["hardware_gates"];
    gates = Json::Value(Json::arrayValue);
    std::ostringstream out;

    for (const auto &endpoint : endpoints["physical"]) {
        const auto &binding = endpoint["binding"];
        uint32_t dai = binding["dai"].asUInt();
        uint32_t module = endpoint["module"].asUInt();
        std::string direction = endpoint["direction"].asString();
        const auto &widget = widgets["widgets"][std::to_string(dai)][direction];
        if (!widget.isString() || endpoints["physical"].size() != 1) {
            Json::Value problem;
            problem["kind"] = !widget.isString() ?
                "compiler-hardware-widget-missing" :
                "compiler-hardware-multiple-endpoints";
            problem["scope"] = "compiler-coverage";
            problem["object"] = module;
            problem["detail"] = dai;
            compiled.report["issues"].append(problem);
            continue;
        }

        std::string marker = "ar_hw" + std::to_string(module);
        std::string control = marker + "_on";
        std::string module_widget = "m" + std::to_string(module);
        std::string port = widget.asString();
        dapm_switch(compiled.topology, marker, graph_id);
        out << "SectionGraph.\"" << marker << "\" {\n    index \"0\"\n"
            << "    lines [\n";

        if (direction == "playback") {
            out << "        \"" << marker << ", " << control << ", "
                << module_widget << "\"\n"
                << "        \"" << port << ", , " << marker << "\"\n";
        } else {
            out << "        \"" << marker << ", " << control << ", "
                << port << "\"\n"
                << "        \"" << module_widget << ", , " << marker
                << "\"\n";
        }

        out << "    ]\n}\n\n";
        Json::Value gate;
        gate["graph_id"] = graph_id;
        gate["module"] = module;
        gate["dai"] = dai;
        gate["direction"] = direction;
        gate["hardware_widget"] = port;
        gate["marker"] = marker;
        gate["control"] = marker + " " + control;
        const auto &backend = widgets["backends"][std::to_string(dai)][direction];
        if (!backend.isNull())
            gate["backend"] = backend;

        gates.append(gate);
    }

    compiled.report["can_emit"] = compiled.report["issues"].empty();

    if (compiled.report["can_emit"].asBool())
        compiled.topology.body += out.str();
    else
        compiled.topology = {};
}

}
