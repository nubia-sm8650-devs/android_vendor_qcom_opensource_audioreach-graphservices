/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_bindings.h"

#include <cassert>
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <json/json.h>

static Json::Value fixture()
{
    Json::Value input;
    input["version"] = 1;
    input["frontends"] = Json::Value(Json::arrayValue);
    input["references"] = Json::Value(Json::arrayValue);
    input["selections"] = Json::Value(Json::arrayValue);
    input["hardware_gates"] = Json::Value(Json::arrayValue);

    for (unsigned id : {1, 2, 3}) {
        Json::Value selection;
        Json::Value pair(Json::arrayValue);
        pair.append("0xa");
        pair.append(std::to_string(id));
        selection["graph_keys"].append(pair);
        selection["graphs"].append(id);
        Json::Value tuning;
        tuning["graph_id"] = id;
        tuning["subgraph"] = 0xb0000000u + id;
        tuning["set_control"] = "set" + std::to_string(id);
        tuning["modules"].append(0x4000 + id);
        selection["tuning"].append(tuning);
        selection["references"] = Json::Value(Json::arrayValue);

        if (id == 1)
            selection["references"].append("link");

        input["selections"].append(selection);
    }

    Json::Value frontend;
    frontend["pcm_id"] = 0;
    frontend["graph_id"] = 1;
    frontend["direction"] = "playback";
    frontend["mode"] = "pcm";
    input["frontends"].append(frontend);
    Json::Value reference;
    reference["name"] = "link";
    reference["control"] = false;
    reference["source_graph"] = 1;
    reference["destination_graph"] = 2;
    reference["path_control"] = "path";
    input["references"].append(reference);
    Json::Value hardware;
    hardware["graph_id"] = 2;
    hardware["control"] = "hardware";
    input["hardware_gates"].append(hardware);
    return input;
}

static void emitted(const char *path)
{
    std::ifstream file(path);
    Json::Value input;
    file >> input;
    ar_topology::Bindings bindings(input);

    for (const auto &selection : input["selections"]) {
        ar_topology::Keys keys;

        for (const auto &pair : selection["graph_keys"])
            keys.emplace_back(std::stoul(pair[0].asString(), nullptr, 0),
                              std::stoul(pair[1].asString(), nullptr, 0));

        ar_topology::Plan plan;
        int status = bindings.select({keys}, plan);

        if (!selection["ownership_bound"].asBool()) {
            assert(status == -EOPNOTSUPP);
            continue;
        }

        assert(!status);
        std::set<unsigned> graphs;
        std::map<uint32_t, uint32_t> modules;
        std::set<std::string> references, paths;

        for (const auto &graph : selection["graphs"])
            graphs.insert(graph.asUInt());

        for (const auto &[id, graph] : plan.graphs) {
            assert(graphs.erase(id));
            bool matched = false;

            for (const auto &tuning : selection["tuning"])
                matched |= tuning["graph_id"].asUInt() == id &&
                    tuning["subgraph"].asUInt() == graph.subgraph &&
                    tuning["set_control"].asString() == graph.set_control;

            assert(matched);
        }

        assert(graphs.empty());

        for (const auto &tuning : selection["tuning"])
            for (const auto &module : tuning["modules"])
                modules.emplace(module.asUInt(), tuning["graph_id"].asUInt());

        assert(plan.modules == modules);
        std::sort(keys.begin(), keys.end());
        assert(plan.selections.size() == 1);
        assert(plan.selections.count(keys));

        for (const auto &reference : selection["references"])
            references.insert(reference.asString());

        for (const auto &path_control : selection["paths"])
            paths.insert(path_control.asString());

        assert(plan.references == references);
        assert(plan.paths == paths);
    }
}

static Json::Value device_selection(unsigned device,
                                    std::vector<unsigned> graphs,
                                    std::vector<std::string> references)
{
    Json::Value selection;

    for (auto [key, value] : {std::pair{10u, 1u}, {20u, device}}) {
        Json::Value pair(Json::arrayValue);
        pair.append(std::to_string(key));
        pair.append(std::to_string(value));
        selection["graph_keys"].append(pair);
    }

    for (unsigned graph : graphs) {
        selection["graphs"].append(graph);
        Json::Value tuning;
        tuning["graph_id"] = graph;
        tuning["subgraph"] = 0xb0000000u + graph;
        tuning["set_control"] = "set" + std::to_string(graph);
        tuning["modules"].append(0x4000 + graph);
        selection["tuning"].append(tuning);
    }

    selection["references"] = Json::Value(Json::arrayValue);

    for (const auto &name : references)
        selection["references"].append(name);

    return selection;
}

static void partial_frontends()
{
    auto input = fixture();
    input["selections"] = Json::Value(Json::arrayValue);
    input["selections"].append(device_selection(1, {1, 2}, {"link"}));
    Json::Value reference = input["references"][0];
    reference["name"] = "link3";
    reference["destination_graph"] = 3;
    reference["path_control"] = "path3";
    input["references"].append(reference);
    input["selections"].append(device_selection(2, {1, 3}, {"link3"}));
    input["selections"].append(device_selection(4, {2}, {}));

    std::vector<ar_topology::Frontend> frontends;
    ar_topology::Bindings bindings(input);
    assert(!bindings.frontends({{10, 1}}, frontends));
    assert(frontends.size() == 1 && frontends[0].pcm_id == 0 &&
           frontends[0].graph_id == 1 && !frontends[0].capture);
    frontends.clear();
    assert(!bindings.frontends({{20, 2}, {10, 1}}, frontends));
    assert(frontends.size() == 1);
    assert(bindings.frontends({{10, 9}}, frontends) == -ENOENT);
    assert(bindings.frontends({{10, 1}, {10, 2}}, frontends) == -EINVAL);

    Json::Value frontend = input["frontends"][0];
    frontend["pcm_id"] = 5;
    frontend["graph_id"] = 4;
    input["frontends"].append(frontend);
    input["selections"].append(device_selection(3, {4}, {}));
    ar_topology::Bindings ambiguous(input);
    frontends.clear();
    assert(ambiguous.frontends({{10, 1}}, frontends) == -EOPNOTSUPP);
    assert(frontends.empty());
    assert(!ambiguous.frontends({{10, 1}, {20, 3}}, frontends));
    assert(frontends.size() == 1 && frontends[0].pcm_id == 5);
}

static void get_controls()
{
    auto input = fixture();
    ar_topology::Plan plan;

    input["selections"][0]["tuning"][0]["get_control"] = "get1";
    ar_topology::Bindings bindings(input);
    assert(!bindings.select({{{10, 1}}}, plan));
    assert(plan.graphs.at(1).get_control == "get1");
    assert(!bindings.select({{{10, 1}}, {{10, 2}}}, plan));
    assert(plan.graphs.at(2).get_control.empty());

    for (const char *name : {"set1", "set2"}) {
        bool rejected = false;

        input["selections"][0]["tuning"][0]["get_control"] = name;

        try {
            ar_topology::Bindings invalid(input);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }

        assert(rejected);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        emitted(argv[1]);
        return 0;
    }

    partial_frontends();
    get_controls();

    auto input = fixture();
    ar_topology::Bindings bindings(input);
    ar_topology::Plan plan;
    assert(!bindings.select({{{10, 1}}}, plan));
    assert(plan.graphs.size() == 1 && plan.graphs.count(1));
    assert((plan.modules == std::map<uint32_t, uint32_t>{{0x4001, 1}}));
    assert((plan.selections.at({{10, 1}}) == std::set<uint32_t>{1}));
    assert(plan.references == std::set<std::string>{"link"});
    assert(plan.paths.empty() && plan.hardware.empty());
    assert(bindings.select({{{10, 2}}}, plan) == -EOPNOTSUPP);
    assert(plan.graphs.size() == 1 && plan.graphs.count(1));
    assert(!bindings.select({{{10, 1}}, {{10, 2}}, {{10, 1}}}, plan));
    assert(plan.graphs.size() == 2 && plan.graphs.count(2));
    assert((plan.modules == std::map<uint32_t, uint32_t>{{0x4001, 1},
                                                        {0x4002, 2}}));
    assert(plan.selections.size() == 2);
    assert(plan.paths == std::set<std::string>{"path"});
    assert(plan.hardware == std::set<std::string>{"hardware"});
    assert(bindings.select({{{10, 1}}, {{10, 3}}}, plan) == -EOPNOTSUPP);
    assert(bindings.select({{{10, 99}}}, plan) == -ENOENT);
    assert(bindings.select({{{10, 1}, {10, 1}}}, plan) == -EINVAL);
    assert(plan.graphs.size() == 2);

    input["frontends"][0]["direction"] = "capture";
    input["references"][0]["source_graph"] = 2;
    input["references"][0]["destination_graph"] = 1;
    ar_topology::Bindings capture(input);
    assert(!capture.select({{{10, 1}}, {{10, 2}}}, plan));
    assert(plan.frontends.size() == 1 && plan.frontends[0].capture);
    assert(plan.frontends[0].mode == ar_topology::FrontendMode::pcm);

    for (const char *mode : {"hostless", "compress", "mmap"}) {
        auto modal = input;
        modal["frontends"][0]["mode"] = mode;
        bool accepted = true;

        try {
            ar_topology::Bindings bindings(modal);
            assert(!bindings.select({{{10, 1}}, {{10, 2}}}, plan));
            assert(plan.frontends[0].mode ==
                   (std::string(mode) == "hostless" ?
                    ar_topology::FrontendMode::hostless :
                    ar_topology::FrontendMode::compress));
        } catch (const std::invalid_argument &) {
            accepted = false;
        }

        assert(accepted == (std::string(mode) != "mmap"));
    }

    auto duplex = fixture();
    duplex["frontends"][0]["mode"] = "hostless";
    duplex["frontends"].append(duplex["frontends"][0]);
    duplex["frontends"][1]["direction"] = "capture";
    Json::Value feed;
    feed["name"] = "feed";
    feed["control"] = false;
    feed["source_graph"] = 3;
    feed["destination_graph"] = 1;
    feed["path_control"] = "feed path";
    duplex["references"].append(feed);
    duplex["selections"][2]["references"].append("feed");
    ar_topology::Bindings loopback(duplex);
    assert(!loopback.select({{{10, 1}}, {{10, 2}}, {{10, 3}}}, plan));
    assert(plan.frontends.size() == 2 && plan.frontends[0].pcm_id == 0 &&
           plan.frontends[1].pcm_id == 0 &&
           plan.frontends[0].capture != plan.frontends[1].capture);
    assert(plan.frontend_graphs.size() == 1 &&
           plan.frontend_graphs.at(0) == std::set<uint32_t>({1, 2, 3}));

    for (int change = 0; change < 4; change++) {
        auto invalid = duplex;
        auto &second = invalid["frontends"][1];

        if (change == 0)
            invalid["frontends"][0]["mode"] = second["mode"] = "pcm";
        else if (change == 1)
            second["graph_id"] = 3;
        else if (change == 2)
            second["pcm_id"] = 1;
        else
            second["direction"] = "playback";

        bool rejected = false;

        try {
            ar_topology::Bindings bindings(invalid);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }

        assert(rejected);
    }

    Json::Value extra(Json::arrayValue);
    extra.append("0x1");
    extra.append("0x2");
    input["selections"][0]["graph_keys"].append(extra);
    ar_topology::Bindings complete(input);
    assert(complete.select({{{10, 1}}}, plan) == -ENOENT);
    assert(!complete.select({{{10, 1}, {1, 2}}, {{10, 2}}}, plan));
    assert(!complete.select({{{1, 2}, {10, 1}}, {{10, 2}}}, plan));

    input["selections"][0]["graph_keys"][0][1] = "0xfffffffff";
    bool rejected = false;

    try {
        ar_topology::Bindings invalid(input);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }

    assert(rejected);

    input = fixture();
    auto &gate = input["hardware_gates"][0];
    gate["module"] = 0x4002;
    gate["dai"] = 16;
    gate["direction"] = "playback";
    auto &backend = gate["backend"];
    backend["dai"] = 16;
    backend["provider"] = "/dais";
    backend["path"] = "/sound/speaker";
    backend["name"] = "Speaker Playback";
    backend["aif"]["id"] = 9;
    backend["aif"]["name"] = "native playback";
    for (const char *key : {"rate", "format", "channels", "data_format"})
        backend["controls"][key] = std::string("explicit ") + key;

    ar_topology::Bindings media(input);
    assert(!media.select({{{10, 1}}, {{10, 2}}}, plan));
    const auto &native = plan.backends.at("hardware");
    assert((native.id == ar_topology::BackendId{"/dais", 16}));
    assert(native.graph == 2 && native.module == 0x4002 && !native.capture);
    assert(native.path == "/sound/speaker" && native.name == "Speaker Playback");
    assert(native.controls.at("data_format") == "explicit data_format");
    assert(media.aifs().size() == 1);
    assert(media.aifs().at(9).aif_name == "native playback");
    auto repeated_input = input;
    repeated_input["hardware_gates"].append(gate);
    repeated_input["hardware_gates"][1]["control"] = "repeated hardware";
    ar_topology::Bindings repeated(repeated_input);
    assert(repeated.aifs().size() == 1);
    assert(!repeated.select({{{10, 1}}, {{10, 2}}}, plan));
    assert(plan.backends.size() == 2);
    for (const char *field : {"id", "name", "group"}) {
        auto conflict = repeated_input;
        auto &aif = conflict["hardware_gates"][1]["backend"]["aif"];
        if (std::string(field) == "id")
            aif[field] = 10;
        else
            aif[field] = "other";
        rejected = false;
        try {
            ar_topology::Bindings invalid(conflict);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    gate["module"] = 0x4003;
    rejected = false;
    try {
        ar_topology::Bindings invalid(input);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);

    for (bool missing : {false, true}) {
        input = fixture();

        if (missing)
            input["selections"][0]["tuning"][0].removeMember("modules");
        else
            input["selections"][1]["tuning"][0]["modules"][0] = 0x4001;

        rejected = false;

        try {
            ar_topology::Bindings invalid(input);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }

        assert(rejected);
    }
}
