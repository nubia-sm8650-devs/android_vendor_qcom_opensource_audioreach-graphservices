/* SPDX-License-Identifier: BSD-3-Clause */
#include "compiler.h"

#include <limits>
#include <sstream>
#include <stdexcept>
#include <sound/snd_ar_tokens.h>

namespace acdb_topology
{

void append_word(Bytes &data, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        data.push_back(value >> shift);
}

void private_data(std::ostream &out, const std::string &name,
                  uint32_t type, const Bytes &payload)
{
    if (payload.size() > UINT32_MAX - 8)
        throw std::runtime_error("Private data block exceeds topology size");

    Bytes data;
    append_word(data, 8 + payload.size());
    append_word(data, type);
    data.insert(data.end(), payload.begin(), payload.end());

    std::string bytes = hex(data);
    out << "SectionData.\"" << name << "\" {\n    bytes \"";

    for (size_t i = 0; i < bytes.size(); i += 2)
        out << (i ? ", " : "") << "0x" << bytes.substr(i, 2);

    out << "\"\n}\n\n";
}

void control_data(std::ostream &out, const std::string &name,
                  const std::vector<Connection> &links)
{
    Bytes data;
    append_word(data, links.size());

    for (const auto &link : links)
        data.insert(data.end(), link.bytes.begin(), link.bytes.end());

    private_data(out, name, SND_SOC_AR_TPLG_CONTROL_LINK_TYPE, data);
}

void tuples(std::ostream &out, const std::string &name, const Tokens &tokens)
{
    out << "SectionVendorTuples.\"" << name << "\" {\n"
        << "    tokens \"ar\"\n    tuples.\"word\" {\n";

    for (auto [token, value] : tokens)
        out << "        t" << token << " \"" << value << "\"\n";

    out << "    }\n}\nSectionData.\"" << name << "\" {\n"
        << "    tuples \"" << name << "\"\n}\n\n";
}

std::string Topology::render() const
{
    std::ostringstream out;
    out << "SectionVendorTokens.\"ar\" {\n";

    for (uint32_t token : tokens)
        out << "    t" << token << " \"" << token << "\"\n";

    out << "}\n\n" << body;

    return out.str();
}

Json::Value parameter_controls(Topology &topology, uint32_t graph_id)
{
    std::ostringstream out;
    Json::Value result;
    std::string prefix = "ar_g" + std::to_string(graph_id);
    std::string data = prefix + "_params";
    tuples(out, data, {{AR_TKN_DAI_INDEX, graph_id}});
    topology.tokens.insert(AR_TKN_DAI_INDEX);

    for (const char *operation : {"set", "get"}) {
        bool get = std::string(operation) == "get";
        std::string name = prefix + "_" + operation;
        uint32_t opcode = get ? SND_SOC_AR_TPLG_GET_PARAMS :
                                SND_SOC_AR_TPLG_SET_PARAMS;
        result[operation] = name;
        out << "SectionControlBytes.\"" << name << "\" {\n"
            << "    max \"" << std::numeric_limits<int32_t>::max() << "\"\n"
            << "    access [ \"tlv_write\" \"tlv_callback\"";

        if (get)
            out << " \"tlv_read\"";

        out << " ]\n    extops.\"ctl\" { put \"" << opcode << "\"";

        if (get)
            out << " get \"" << opcode << "\"";

        out << " }\n    data [ \"" << data << "\" ]\n}\n\n";
    }

    topology.body += out.str();

    return result;
}

void dapm_switch(Topology &topology, const std::string &name,
                  uint32_t graph_id)
{
    std::ostringstream out;
    std::string control = name + "_on";
    std::string data = name + "_graph";
    tuples(out, data, {{AR_TKN_DAI_INDEX, graph_id}});
    topology.tokens.insert(AR_TKN_DAI_INDEX);
    out << "SectionControlMixer.\"" << control << "\" {\n"
        << "    max \"1\"\n    no_pm \"true\"\n"
        << "    channel.\"fl\" { reg \"-1\" shift \"0\" }\n"
        << "    channel.\"fr\" { reg \"-1\" shift \"0\" }\n"
        << "    ops.\"ctl\" { get \"64\" put \"64\" info \"1\" }\n"
        << "}\n\nSectionWidget.\"" << name << "\" {\n"
        << "    type \"mixer\"\n    no_pm \"true\"\n"
        << "    data [ \"" << data << "\" ]\n"
        << "    mixer [ \"" << control << "\" ]\n}\n\n";
    topology.body += out.str();
}

}
