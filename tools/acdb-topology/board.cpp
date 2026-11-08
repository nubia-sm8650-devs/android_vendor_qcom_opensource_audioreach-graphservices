/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <libfdt.h>
#include <libxml/parser.h>

namespace acdb_topology
{
namespace
{

std::string trim(const std::string &value)
{
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};

    return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}

std::vector<std::string> split(const std::string &value, char separator)
{
    std::vector<std::string> result;
    std::istringstream stream(value);
    std::string part;

    while (std::getline(stream, part, separator))
        result.push_back(trim(part));

    return result;
}

bool named(const xmlNode *node, const char *name)
{
    return node->type == XML_ELEMENT_NODE &&
           xmlStrEqual(node->name, BAD_CAST name);
}

xmlNode *child(xmlNode *node, const char *name)
{
    for (auto *item = node->children; item; item = item->next) {
        if (named(item, name))
            return item;
    }

    throw std::runtime_error(std::string("Missing XML element ") + name);
}

std::string attribute(xmlNode *node, const char *name)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    if (!value)
        throw std::runtime_error(std::string("Missing XML attribute ") + name);

    std::string result(reinterpret_cast<const char *>(value));
    xmlFree(value);

    return result;
}

std::string content(xmlNode *node)
{
    xmlChar *value = xmlNodeGetContent(node);
    std::string result(reinterpret_cast<const char *>(value));
    xmlFree(value);

    return trim(result);
}

std::string xml_text(xmlNode *node)
{
    xmlBufferPtr buffer = xmlBufferCreate();
    if (!buffer)
        throw std::runtime_error("Cannot allocate XML buffer");

    xmlNodeDump(buffer, node->doc, node, 0, 0);
    auto *text = reinterpret_cast<const char *>(xmlBufferContent(buffer));
    std::string result(text);
    xmlBufferFree(buffer);

    return result;
}

using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

Document document(const std::string &path, const char *root)
{
    Document result(xmlReadFile(path.c_str(), nullptr, XML_PARSE_NONET),
                     xmlFreeDoc);
    if (!result || !xmlDocGetRootElement(result.get()) ||
        !named(xmlDocGetRootElement(result.get()), root))
        throw std::runtime_error("Invalid XML " + path);

    return result;
}

Json::Value transport(const char *family, const char *direction,
                       uint32_t bank, uint32_t index)
{
    Json::Value result;
    result["family"] = family;
    result["direction"] = direction;
    result["bank"] = bank;
    result["index"] = index;

    return result;
}

Json::Value backend_transport(const std::string &backend)
{
    auto parts = split(backend, '-');
    if (parts.size() == 2 && parts[0] == "USB_AUDIO" &&
        (parts[1] == "RX" || parts[1] == "TX")) {
        Json::Value result;
        result["family"] = "USB_AUDIO";
        result["direction"] = parts[1] == "RX" ? "playback" : "capture";

        return result;
    }

    if (parts.size() != 4 || (parts[2] != "RX" && parts[2] != "TX"))
        return {};

    const char *direction = parts[2] == "RX" ? "playback" : "capture";

    if (parts[0] == "SLIM" &&
        (parts[1] == "DEV1" || parts[1] == "DEV2")) {
        Json::Value result;
        result["family"] = "SLIM";
        result["direction"] = direction;
        result["device"] = parts[1] == "DEV1" ? 0 : 1;
        result["backend_port"] = read_number(parts[3]);

        return result;
    }

    if (parts[0] == "MI2S" && parts[1] == "LPAIF") {
        const std::vector<std::string> indices{
            "PRIMARY", "SECONDARY", "TERTIARY", "QUATERNARY",
            "QUINARY", "SENARY"};

        for (size_t i = 0; i < indices.size(); i++) {
            if (parts[3] == indices[i])
                return transport("MI2S", direction, 0, i);
        }
    }

    if (parts[0] == "CODEC_DMA") {
        uint32_t bank;

        if (parts[1] == "LPAIF_RXTX")
            bank = 1;
        else if (parts[1] == "LPAIF_WSA")
            bank = 2;
        else if (parts[1] == "LPAIF_VA")
            bank = 3;
        else
            return {};

        uint32_t index = read_number(parts[3]);
        if (index < 8)
            return transport("CODEC_DMA", direction, bank, index + 1);
    }

    return {};
}

Json::Value dai_transport(uint32_t id)
{
    if (id == 136 || id == 153) {
        Json::Value result;
        result["family"] = "USB_AUDIO";
        result["direction"] = id == 136 ? "playback" : "capture";

        return result;
    }

    if (id >= 2 && id <= 15) {
        Json::Value result;
        result["family"] = "SLIM";
        result["direction"] = id % 2 ? "capture" : "playback";
        result["port"] = (id - 2) / 2;

        return result;
    }

    if (id >= 16 && id <= 23)
        return transport("MI2S", id % 2 ? "capture" : "playback",
                           0, (id - 16) / 2);

    if (id == 127 || id == 128 || id == 147 || id == 148)
        return transport("MI2S", id % 2 ? "playback" : "capture",
                           0, id < 129 ? 4 : 5);

    if (id == 105 || id == 107)
        return transport("CODEC_DMA", "playback", 2, (id - 105) / 2 + 1);

    if (id == 106 || id == 108 || id == 109)
        return transport("CODEC_DMA", "capture", 2, id == 106 ? 1 : id - 106);

    if (id >= 110 && id <= 112)
        return transport("CODEC_DMA", "capture", 3, id - 109);

    if (id >= 113 && id <= 124)
        return transport("CODEC_DMA", id % 2 ? "playback" : "capture",
                           1, (id - 113) / 2 + 1);

    if (id == 125 || id == 126)
        return transport("CODEC_DMA", "playback", 1, id - 118);

    return {};
}

bool matching_transport(const Json::Value &profile, const Json::Value &link)
{
    if (profile.isNull() || link.isNull())
        return false;

    if (profile["family"] == "SLIM")
        return link["family"] == "SLIM" &&
               profile["direction"] == link["direction"];

    return profile == link;
}

std::string dt_string(const void *dtb, int node, const char *name)
{
    int size;
    auto *value = static_cast<const char *>(
        fdt_getprop(dtb, node, name, &size));
    if (!value)
        return {};

    if (size <= 0 || value[size - 1])
        throw std::runtime_error("Invalid DT string");

    return value;
}

bool available(const void *dtb, int node)
{
    for (; node >= 0; node = fdt_parent_offset(dtb, node)) {
        std::string status = dt_string(dtb, node, "status");
        if (!status.empty() && status != "okay" && status != "ok")
            return false;
    }

    return true;
}

std::string dt_path(const void *dtb, int node)
{
    std::vector<char> buffer(fdt_totalsize(dtb));
    if (fdt_get_path(dtb, node, buffer.data(), buffer.size()))
        throw std::runtime_error("Cannot read DT path");

    return buffer.data();
}

Json::Value board_links(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read DTB " + path);

    Bytes bytes{std::istreambuf_iterator<char>(input), {}};
    const void *dtb = bytes.data();
    if (fdt_check_full(dtb, bytes.size()))
        throw std::runtime_error("Invalid DTB " + path);

    Json::Value result(Json::arrayValue);
    int node = -1;

    while ((node = fdt_next_node(dtb, node, nullptr)) >= 0) {
        std::string name = dt_string(dtb, node, "link-name");
        int cpu = fdt_subnode_offset(dtb, node, "cpu");
        if (name.empty() || cpu < 0 || !available(dtb, cpu))
            continue;

        int size;
        auto *dai = static_cast<const fdt32_t *>(
            fdt_getprop(dtb, cpu, "sound-dai", &size));
        if (!dai || size != 8)
            continue;

        int provider = fdt_node_offset_by_phandle(dtb, fdt32_to_cpu(dai[0]));
        if (provider < 0 || !available(dtb, provider) ||
            fdt_node_check_compatible(dtb, provider, "qcom,q6apm-lpass-dais"))
            continue;

        auto *cells = static_cast<const fdt32_t *>(
            fdt_getprop(dtb, provider, "#sound-dai-cells", &size));
        if (!cells || size != 4 || fdt32_to_cpu(*cells) != 1)
            throw std::runtime_error("Invalid LPASS sound-dai cells");

        uint32_t id = fdt32_to_cpu(dai[1]);
        Json::Value link;
        link["path"] = dt_path(dtb, node);
        link["name"] = name;
        link["provider"] = dt_path(dtb, provider);
        link["dai"] = id;
        link["transport"] = dai_transport(id);
        result.append(link);
    }

    return result;
}

}

Json::Value read_board(const std::string &resources,
                       const std::string &usecases, const std::string &dtb)
{
    auto rm = document(resources, "resource_manager_info");
    auto kv = document(usecases, "graph_key_value_pair_info");
    Json::Value result;
    result["resource_manager"] = resources;
    result["usecase_kv"] = usecases;
    result["dtb"] = dtb;
    result["links"] = board_links(dtb);
    result["profiles"] = Json::Value(Json::arrayValue);
    result["assignments"] = Json::Value(Json::arrayValue);
    auto *profiles = child(xmlDocGetRootElement(rm.get()), "device_profile");

    for (auto *node = profiles->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE)
            continue;

        Json::Value profile;
        profile["device"] = content(child(node, "id"));
        profile["backend"] = content(child(node, "back_end_name"));
        profile["xml"] = xml_text(node);
        profile["transport"] = backend_transport(profile["backend"].asString());
        profile["links"] = Json::Value(Json::arrayValue);

        for (Json::ArrayIndex i = 0; i < result["links"].size(); i++) {
            if (matching_transport(profile["transport"],
                                     result["links"][i]["transport"]))
                profile["links"].append(i);
        }

        result["profiles"].append(profile);
    }

    for (const char *section : {"devices", "devicepps"}) {
        auto *entries = child(xmlDocGetRootElement(kv.get()), section);

        for (auto *node = entries->children; node; node = node->next) {
            if (node->type != XML_ELEMENT_NODE)
                continue;

            for (auto *choice = node->children; choice; choice = choice->next) {
                if (!named(choice, "keys_and_values"))
                    continue;

                Json::Value value;
                value["section"] = section;
                value["devices"] = Json::Value(Json::arrayValue);
                value["conditions"] = Json::Value(Json::objectValue);
                value["keys"] = Json::Value(Json::arrayValue);
                value["xml"] = xml_text(choice);

                for (const auto &device : split(attribute(node, "id"), ','))
                    value["devices"].append(device);

                for (auto *attr = choice->properties; attr; attr = attr->next) {
                    auto *name = reinterpret_cast<const char *>(attr->name);
                    value["conditions"][name] = attribute(choice, name);
                }

                for (auto *pair = choice->children; pair; pair = pair->next) {
                    if (!named(pair, "graph_kv"))
                        continue;

                    Json::Value keys(Json::arrayValue);
                    keys.append(read_number(attribute(pair, "key")));
                    keys.append(read_number(attribute(pair, "value")));
                    value["keys"].append(keys);
                }

                result["assignments"].append(value);
            }
        }
    }

    return result;
}

}
