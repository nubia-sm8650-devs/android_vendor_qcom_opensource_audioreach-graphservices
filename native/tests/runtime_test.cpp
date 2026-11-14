/* SPDX-License-Identifier: BSD-3-Clause */
#include "fake_card.h"

static ar_topology::Pcm configuration(unsigned id)
{
    pcm_config config{};
    config.channels = 2;
    config.rate = 48000;
    config.period_size = 240;
    config.period_count = 4;
    config.format = PCM_FORMAT_S16_LE;
    return {id, unsigned(id == 4 ? PCM_IN : PCM_OUT), config};
}

struct Initialization {
    mixer &controls;
    std::vector<unsigned> defaults;
    bool reject = false;
};

static int initialize(void *opaque,
                      const std::vector<ar_topology::Stream> &streams) noexcept
{
    auto &value = *static_cast<Initialization *>(opaque);

    for (const auto &stream : streams)
        assert(!stream.handle || stream.handle->state == SNDRV_PCM_STATE_SETUP);

    for (unsigned id : value.defaults) {
        const auto &data = value.controls.controls.at("set" +
                                                       std::to_string(id));
        uint32_t subgraph;
        assert(data.bytes.size() == 32);
        std::memcpy(&subgraph, data.bytes.data() + 24, sizeof(subgraph));
        assert(subgraph == 0xb0000000u + id);
    }

    initialization_seen = true;
    return value.reject ? -ECANCELED : 0;
}

static void media_configuration()
{
    using namespace ar_topology;
    auto input = fixture();
    auto &gate = input["hardware_gates"][0];
    gate["module"] = 0x4003;
    gate["dai"] = 16;
    gate["direction"] = "playback";
    auto &backend = gate["backend"];
    backend["dai"] = 16;
    backend["provider"] = "/dais";
    backend["path"] = "/sound/speaker";
    backend["name"] = "Speaker Playback";
    for (const char *key : {"rate", "format", "channels", "data_format"})
        backend["controls"][key] = std::string("backend ") + key;

    Bindings bindings(input);
    auto controls = make_mixer();
    controls.controls["backend rate"].enums = {"96000", "48000"};
    controls.controls["backend channels"].enums = {"1", "2"};
    controls.controls["backend format"].enums = {"S32_LE", "S16_LE"};
    controls.controls["backend data_format"].enums = {
        "Topology", "Compressed over PCM", "Fixed Point"};
    Runtime runtime(0, &controls, bindings);
    backend_mixer = &controls;
    backend_rate = 48000;
    backend_data_format = "Fixed Point";
    Media config{48000, 2, 1, "S16_LE"};
    HardwareConfig first{{{10, 1}}, config}, second{{{10, 2}}, config};
    SessionId a = 0, b = 0;
    std::vector<Stream> x, y;

    first.media.rate = 12345;
    assert(runtime.acquire({first.selection}, {configuration(1)}, {first}, a, x)
           == -EINVAL);
    assert(!a && x.empty() && open_pcms.empty());
    for (const char *key : {"rate", "format", "channels", "data_format"})
        assert(!controls.controls.at(std::string("backend ") + key).enabled);

    first.media = config;
    assert(!runtime.acquire({first.selection}, {configuration(1)}, {first},
                             a, x));
    assert(controls.controls.at("backend rate").enabled == 1);
    assert(controls.controls.at("backend format").enabled == 1);
    assert(controls.controls.at("backend channels").enabled == 1);
    assert(controls.controls.at("backend data_format").enabled == 2);
    assert(runtime.acquire({second.selection}, {configuration(2)}, b, y) ==
           -EBUSY);
    second.media.rate = 96000;
    assert(runtime.acquire({second.selection}, {configuration(2)}, {second},
        b, y)
           == -EBUSY);
    assert(!b && y.empty());
    assert(!controls.controls.at("path2").enabled);
    assert(!controls.controls.at("link2").enabled);
    assert(controls.controls.at("backend rate").enabled == 1);
    second.media = config;
    assert(!runtime.acquire({second.selection}, {configuration(2)}, {second},
                             b, y));
    assert(!runtime.close(a));
    a = 0;
    x.clear();
    first.media.rate = 96000;
    assert(runtime.acquire({first.selection}, {configuration(1)}, {first}, a, x)
           == -EBUSY);
    assert(!runtime.close(b));
    first.media.data_format = 7;
    backend_rate = 96000;
    backend_data_format = "Compressed over PCM";
    assert(!runtime.acquire({first.selection}, {configuration(1)}, {first},
                             a, x));
    assert(!controls.controls.at("backend rate").enabled);
    assert(!runtime.close(a));
    assert(open_pcms.empty());
    backend_mixer = nullptr;

    Json::Value combined = input["selections"][0];
    combined["graph_keys"][0][1] = "99";
    for (const auto &graph : input["selections"][2]["graphs"])
        combined["graphs"].append(graph);
    for (const auto &graph : input["selections"][2]["tuning"])
        combined["tuning"].append(graph);
    combined["references"].append("link4");
    input["selections"].append(combined);
    auto &other = input["hardware_gates"][1];
    other["module"] = 0x4005;
    other["dai"] = 120;
    other["direction"] = "capture";
    other["backend"] = backend;
    other["backend"]["dai"] = 120;
    other["backend"]["path"] = "/sound/other";
    for (const char *key : {"rate", "format", "channels", "data_format"})
        other["backend"]["controls"][key] = std::string("other ") + key;
    Bindings duplex(input);
    Runtime ambiguous(0, &controls, duplex);
    a = 0;
    x.clear();
    HardwareConfig unspecified{{{10, 99}}, config};
    assert(ambiguous.acquire({unspecified.selection},
        {configuration(1), configuration(4)}, {unspecified}, a, x) ==
        -EOPNOTSUPP);
    assert(!a && x.empty() && open_pcms.empty());

    controls.controls["other rate"].enums = {"96000", "16000"};
    controls.controls["other channels"].enums = {"1", "2"};
    controls.controls["other format"].enums = {"S32_LE", "S16_LE"};
    controls.controls["other data_format"].enums = {
        "Topology", "Compressed over PCM", "Fixed Point"};
    HardwareConfig speaker{{{10, 99}}, config, {"/dais", 16}};
    HardwareConfig microphone{{{10, 99}}, {16000, 1, 1, "S16_LE"},
                              {"/dais", 120}};
    assert(!ambiguous.acquire({speaker.selection},
        {configuration(1), configuration(4)}, {speaker, microphone}, a, x));
    assert(controls.controls.at("backend rate").enabled == 1);
    assert(controls.controls.at("backend channels").enabled == 1);
    assert(controls.controls.at("other rate").enabled == 1);
    assert(!controls.controls.at("other channels").enabled);
    assert(!ambiguous.close(a));
    a = 0;
    x.clear();
    assert(ambiguous.acquire({speaker.selection},
        {configuration(1), configuration(4)}, {speaker}, a, x) == -EINVAL);
    assert(!a && x.empty() && open_pcms.empty());

    backend["controls"].removeMember("channels");
    Bindings without_channels(input);
    Runtime missing_channels(0, &controls, without_channels);
    a = 0;
    x.clear();
    assert(missing_channels.acquire({first.selection}, {configuration(1)},
        {first}, a, x) == -EOPNOTSUPP);

    gate["dai"] = backend["dai"] = 14;
    backend["controls"]["channel_map"] = "backend channel_map";
    auto &map = controls.controls["backend channel_map"].integers;
    map = {1, 157, 0};
    Bindings codec(input);
    Runtime mapped(0, &controls, codec);
    HardwareConfig mono{{{10, 1}}, {48000, 1, 1, "S16_LE"}, {"/dais", 16}};
    assert(mapped.acquire({mono.selection}, {configuration(1)}, {mono}, a, x) ==
           -EINVAL);
    mono.backend.second = 14;
    mono.media.channels = 2;
    assert(mapped.acquire({mono.selection}, {configuration(1)}, {mono}, a, x) ==
           -EINVAL);
    mono.media.channels = 1;
    assert(!mapped.acquire({mono.selection}, {configuration(1)}, {mono}, a, x));
    Initialization tuning{controls, {1, 3}};
    map[1] = 158;
    assert(mapped.prepare(a, initialize, &tuning) == -ESTALE);
    assert(x[0].handle->state == SNDRV_PCM_STATE_SETUP);
    map[1] = 157;
    assert(!mapped.prepare(a, initialize, &tuning));
    map[1] = 158;
    assert(mapped.start(a) == -ESTALE);
    assert(x[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    map[1] = 157;
    assert(!mapped.start(a));
    map[1] = 158;
    mono.selection = {{10, 2}};
    b = 0;
    y.clear();
    assert(mapped.acquire({mono.selection}, {configuration(2)}, {mono}, b, y) ==
           -ESTALE);
    assert(!b && y.empty());
    assert(!mapped.close(a));
    assert(!mapped.acquire({mono.selection}, {configuration(2)}, {mono}, b, y));
    assert(!mapped.close(b));
    assert(open_pcms.empty());
    a = 0;
    x.clear();

    gate.removeMember("backend");
    Bindings unqualified(input);
    Runtime missing_binding(0, &controls, unqualified);
    assert(missing_binding.acquire({first.selection}, {configuration(1)},
        {first}, a, x) == -EOPNOTSUPP);
    assert(!a && x.empty() && open_pcms.empty());
}

static void calibration()
{
    ar_topology::Bindings bindings(fixture());
    auto controls = make_mixer();
    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId id = 0;
    std::vector<ar_topology::Stream> streams;
    calibrated.clear();

    assert(runtime.acquire({{{10, 1}}}, {configuration(1)}, id, streams,
                           {{{50, 5}}, {{50, 6}}}) == -EINVAL);
    assert(!id && streams.empty() && calibrated.empty());
    assert(!runtime.acquire({{{10, 1}}}, {configuration(1)}, id, streams,
                            {{{50, 5}}}));

    for (uint32_t subgraph : {0xb0000001u, 0xb0000003u}) {
        const auto &[prior, next] = calibrated.at(subgraph);
        assert(prior.empty() && next == ar_topology::Keys({{50, 5}}));
        const auto &bytes = controls.controls.at(
            "set" + std::to_string(subgraph & 0xff)).bytes;
        uint32_t value;
        std::memcpy(&value, bytes.data() + 28, sizeof(value));
        assert(value == 5);
    }

    calibrated.clear();
    ar_topology::SessionId other = 0;
    std::vector<ar_topology::Stream> others;
    assert(!runtime.acquire({{{10, 2}}}, {configuration(2)}, other, others));
    assert(calibrated.size() == 1);
    assert(calibrated.at(0xb0000002).first.empty() &&
           calibrated.at(0xb0000002).second.empty());
    calibrated.clear();
    assert(!runtime.set_calibration(id, {}, {{50, 7}}));
    assert(calibrated.size() == 2);

    for (uint32_t subgraph : {0xb0000001u, 0xb0000003u}) {
        const auto &[prior, next] = calibrated.at(subgraph);
        assert(prior == ar_topology::Keys({{50, 5}}) &&
               next == ar_topology::Keys({{50, 7}}));
    }

    calibrated.clear();
    assert(runtime.set_calibration(id, {{10, 1}}, {{50, 8}, {51, 1}}) ==
           -EINVAL);
    assert(runtime.set_calibration(id, {{10, 2}}, {{50, 8}}) == -EINVAL);
    assert(calibrated.empty());
    controls.controls.at("set3").reject_parameters = true;
    assert(runtime.set_calibration(id, {{10, 1}}, {{50, 8}}));
    controls.controls.at("set3").reject_parameters = false;
    calibrated.clear();
    assert(!runtime.set_calibration(id, {{10, 1}}, {{50, 9}}));
    assert(calibrated.at(0xb0000003).first == ar_topology::Keys({{50, 7}}));
    assert(!runtime.close(other));
    assert(!runtime.close(id));
    assert(runtime.set_calibration(id, {}, {{50, 1}}) == -ENOENT);
    assert(open_pcms.empty());
}

static void vcpm_parameters()
{
    ar_topology::Bindings bindings(fixture());
    auto controls = make_mixer();
    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId single = 0, pair = 0;
    std::vector<ar_topology::Stream> a, b;
    uint32_t vsid[] = {4, 0x080011bc, 4, 0, 0x11c05000, 0};
    uint32_t amdb[] = {3, 0x08001026, 0, 0};

    assert(!runtime.acquire({{{10, 1}}}, {configuration(1)}, single, a));
    assert(!runtime.set_parameters(single, vsid, sizeof(vsid)));
    const auto &bytes = controls.controls.at("set1").bytes;
    assert(bytes.size() == 8 + sizeof(vsid));
    assert(!std::memcmp(bytes.data() + 8, vsid, sizeof(vsid)));
    assert(runtime.set_parameters(single, amdb, sizeof(amdb)) == -EOPNOTSUPP);
    assert(!runtime.close(single));

    assert(!runtime.acquire({{{10, 1}}, {{10, 2}}},
                            {configuration(1), configuration(2)}, pair, b));
    controls.controls.at("set1").bytes.clear();
    assert(runtime.set_parameters(pair, vsid, sizeof(vsid)) == -EOPNOTSUPP);
    assert(controls.controls.at("set1").bytes.empty());
    assert(!runtime.close(pair));
}

static void parameter_queries()
{
    auto input = fixture();

    for (auto &selection : input["selections"]) {
        for (auto &tuning : selection["tuning"]) {
            if (tuning["graph_id"].asUInt() == 1)
                tuning["get_control"] = "get1";
        }
    }

    ar_topology::Bindings bindings(input);
    auto controls = make_mixer();
    auto &get = controls.controls["get1"];
    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId session = 0;
    std::vector<ar_topology::Stream> streams;

    get.parameter = true;
    get.reply = {1, 2, 3, 4, 5, 6};
    assert(!runtime.acquire({{{10, 1}}}, {configuration(1)}, session, streams));

    uint32_t query[] = {0x4001, 0x0800113d, 6, 0, 0xaaaaaaaa, 0xbbbbbbbb};

    assert(!runtime.get_parameters(session, query, sizeof(query)));
    assert(query[0] == 0x4001 && query[1] == 0x0800113d && query[2] == 6);
    assert(query[4] == 0x04030201 && query[5] == 0xbbbb0605);

    uint32_t tlv[2], sent[4];

    assert(get.bytes.size() == sizeof(tlv) + sizeof(query));
    std::memcpy(tlv, get.bytes.data(), sizeof(tlv));
    std::memcpy(sent, get.bytes.data() + sizeof(tlv), sizeof(sent));
    assert(tlv[0] == SND_SOC_AR_TLV_GET_PARAMS && tlv[1] == sizeof(query));
    assert(sent[0] == 0x4001 && sent[2] == 6);

    get.reply_error = 1;
    assert(runtime.get_parameters(session, query, sizeof(query)) == -EIO);
    get.reply_error = 0;

    query[0] = 0x4003;
    assert(runtime.get_parameters(session, query, sizeof(query)) ==
           -EOPNOTSUPP);
    query[0] = 0x4002;
    assert(runtime.get_parameters(session, query, sizeof(query)) == -ENOENT);
    query[0] = 0x4001;
    query[2] = 12;
    assert(runtime.get_parameters(session, query, sizeof(query)) == -EINVAL);
    assert(runtime.get_parameters(session, query, 8) == -EINVAL);
    assert(!runtime.close(session));
}

struct Rerouted {
    mixer &controls;
    bool seen = false;
    bool reject = false;
};

static int rerouted(void *opaque, ar_topology::Parameters &writer,
                    const std::vector<ar_topology::Stream> &streams) noexcept
{

    auto &value = *static_cast<Rerouted *>(opaque);
    const auto &controls = value.controls.controls;

    assert(streams.size() == 1 && streams[0].id == 2);
    assert(controls.at("link6").enabled && controls.at("path6").enabled);
    assert(!controls.at("hardware6").enabled);
    assert(controls.at("set6").bytes.size() == 32);

    uint32_t record[] = {0x4006, 0x08001026, 4, 0, 7, 0};
    size_t applied = 0;

    assert(!writer.set(record, sizeof(record), &applied));
    assert(applied == sizeof(record));
    assert(controls.at("set6").bytes.size() == 8 + sizeof(record));
    value.seen = true;

    return value.reject ? -ECANCELED : 0;
}

static void reroutes()
{
    auto input = fixture();
    Json::Value selection, pair(Json::arrayValue), reference;

    pair.append("0xa");
    pair.append("7");
    selection["graph_keys"].append(pair);

    for (unsigned graph : {2, 6}) {
        Json::Value tuning;

        selection["graphs"].append(graph);
        tuning["graph_id"] = graph;
        tuning["subgraph"] = 0xb0000000u + graph;
        tuning["set_control"] = "set" + std::to_string(graph);
        tuning["modules"].append(0x4000 + graph);
        selection["tuning"].append(tuning);
    }

    selection["references"].append("link7");
    input["selections"].append(selection);
    reference["name"] = "link7";
    reference["control"] = false;
    reference["source_graph"] = 2;
    reference["destination_graph"] = 6;
    reference["path_control"] = "path7";
    input["references"].append(reference);

    ar_topology::Bindings bindings(input);
    auto controls = make_mixer();
    controls.controls["link7"];
    controls.controls["path7"];
    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId session = 0, other = 0;
    std::vector<ar_topology::Stream> streams, others;
    Rerouted state{controls};
    Bytes custom{0x10, 0x20};
    auto enabled = [&](std::initializer_list<const char *> names) {
        std::set<std::string> expected(names.begin(), names.end()), actual;

        for (const auto &[name, control] : controls.controls) {
            if (!control.parameter && control.enabled)
                actual.insert(name);
        }

        return actual == expected;
    };

    initialization_seen = true;
    assert(!runtime.open({{{10, 2}}}, {configuration(2)}, session, streams));
    assert(!runtime.start(session));
    auto running = streams[0].handle->state;
    assert(enabled({"link2", "path2", "hardware3"}));
    assert(runtime.reroute(session + 1, {{{10, 6}}}, {}) == -ENOENT);
    assert(runtime.reroute(session, {{{10, 1}}}, {}) == -EXDEV);
    assert(runtime.reroute(session, {{{10, 6}}}, {}, {{}, {}}) == -EINVAL);
    assert(enabled({"link2", "path2", "hardware3"}));

    initialization_seen = true;
    assert(!runtime.open({{{10, 1}}}, {configuration(1)}, other, others));
    assert(runtime.reroute(session, {{{10, 6}}}, {}) == -EOPNOTSUPP);
    assert(!runtime.close(other));
    assert(enabled({"link2", "path2", "hardware3"}));

    controls.controls.at("set2").bytes = custom;
    controls.controls.at("set3").bytes = custom;
    state.reject = true;
    assert(runtime.reroute(session, {{{10, 6}}}, {}, {}, rerouted, &state) ==
           -ECANCELED);
    assert(state.seen);
    assert(enabled({"link2", "path2", "hardware3", "link6", "path6"}));

    state.seen = state.reject = false;
    controls.controls.at("set6").bytes.clear();
    assert(!runtime.reroute(session, {{{10, 6}}}, {}, {}, rerouted, &state));
    assert(state.seen);
    assert(enabled({"link2", "path2", "hardware3", "link6", "path6",
                    "hardware6"}));
    assert(controls.controls.at("set2").bytes == custom);
    assert(controls.controls.at("set3").bytes == custom);
    assert(streams[0].handle->state == running);

    controls.controls.at("set6").bytes = custom;
    assert(!runtime.reroute(session, {{{10, 7}}}, {}));
    assert(enabled({"link7", "path7", "hardware6"}));
    assert(controls.controls.at("set6").bytes == custom);

    controls.controls.at("hardware3").reject_enable = true;
    assert(runtime.reroute(session, {{{10, 2}}}, {}) == -EIO);
    controls.controls.at("hardware3").reject_enable = false;
    controls.controls.at("hardware3").enabled = 0;
    assert(enabled({"link2", "path2"}));
    assert(controls.controls.at("set3").bytes.size() == 32);
    controls.controls.at("set3").bytes = custom;
    assert(!runtime.reroute(session, {{{10, 2}}}, {}));
    assert(enabled({"link2", "path2", "hardware3"}));
    assert(controls.controls.at("set3").bytes == custom);
    assert(streams[0].handle->state == running);

    assert(!runtime.close(session));
    assert(enabled({}));
    assert(open_pcms.empty());

    for (unsigned gate : {0, 2}) {
        auto &value = input["hardware_gates"][gate];
        auto &backend = value["backend"];

        value["module"] = gate ? 0x4006 : 0x4003;
        value["dai"] = 16;
        value["direction"] = "playback";
        backend["dai"] = 16;
        backend["provider"] = "/dais";
        backend["path"] = "/sound/speaker";
        backend["name"] = "Speaker Playback";
        for (const char *key : {"rate", "format", "channels", "data_format"})
            backend["controls"][key] = std::string("backend ") + key;
    }

    ar_topology::Bindings shared(input);
    controls.controls["backend rate"].enums = {"96000", "48000"};
    controls.controls["backend channels"].enums = {"1", "2"};
    controls.controls["backend format"].enums = {"S32_LE", "S16_LE"};
    controls.controls["backend data_format"].enums = {
        "Topology", "Compressed over PCM", "Fixed Point"};
    ar_topology::Runtime managed(0, &controls, shared);
    ar_topology::Media stereo{48000, 2, 1, "S16_LE"}, mono{48000, 1, 1, "S16_LE"};

    session = 0;
    streams.clear();
    assert(!managed.acquire({{{10, 2}}}, {configuration(2)},
                            {{{{10, 2}}, stereo}}, session, streams));
    assert(controls.controls.at("backend channels").enabled == 1);
    assert(controls.controls.at("hardware3").enabled);
    assert(managed.reroute(session, {{{10, 2}}}, {{{{10, 2}}, mono}}) ==
           -EBUSY);
    assert(controls.controls.at("backend channels").enabled == 1);
    assert(!managed.reroute(session, {{{10, 7}}}, {{{{10, 7}}, mono}}));
    assert(controls.controls.at("backend channels").enabled == 0);
    assert(!controls.controls.at("hardware3").enabled);
    assert(controls.controls.at("hardware6").enabled);
    assert(!managed.close(session));
    assert(open_pcms.empty());
}

static void peer_paths()
{
    auto input = fixture();
    Json::Value selection, pair(Json::arrayValue), frontend, tuning;

    pair.append("0xa");
    pair.append("8");
    selection["graph_keys"].append(pair);
    selection["graphs"].append(8);
    tuning["graph_id"] = 8;
    tuning["subgraph"] = 0xb0000008u;
    tuning["set_control"] = "set8";
    tuning["modules"].append(0x4008);
    selection["tuning"].append(tuning);
    selection["references"].append("link8");
    input["selections"].append(selection);
    input["selections"][0]["references"].append("link9");
    frontend["pcm_id"] = 8;
    frontend["graph_id"] = 8;
    frontend["direction"] = "capture";
    frontend["mode"] = "pcm";
    input["frontends"].append(frontend);

    for (unsigned id : {8, 9}) {
        Json::Value reference;

        reference["name"] = "link" + std::to_string(id);
        reference["control"] = false;
        reference["source_graph"] = id == 8 ? 4 : 3;
        reference["destination_graph"] = 8;
        reference["path_control"] = "path" + std::to_string(id);
        input["references"].append(reference);
    }

    ar_topology::Bindings bindings(input);
    auto controls = make_mixer();
    controls.controls["set8"].parameter = true;

    for (const char *name : {"link8", "path8", "link9", "path9"})
        controls.controls[name];

    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId record = 0, call = 0, playback = 0;
    std::vector<ar_topology::Stream> streams, others, played;
    auto recorded = configuration(4);
    auto enabled = [&](const char *name) {
        return controls.controls.at(name).enabled;
    };

    recorded.id = 8;
    assert(!runtime.acquire({{{10, 8}}}, {recorded}, record, streams));
    assert(enabled("link8") && !enabled("path8"));
    assert(!runtime.acquire({{{10, 4}}}, {configuration(4)}, call, others));
    assert(enabled("path8"));
    assert(!runtime.acquire({{{10, 1}}}, {configuration(1)}, playback, played));
    assert(enabled("link9") && !enabled("path9"));
    assert(!runtime.close(playback));
    assert(!runtime.close(call));
    assert(enabled("link8") && !enabled("path8"));
    assert(!runtime.close(record));
    assert(!enabled("link8"));

    record = call = 0;
    streams.clear();
    others.clear();
    assert(!runtime.acquire({{{10, 4}}}, {configuration(4)}, call, others));
    open_requires = &controls.controls.at("path8");
    assert(!runtime.acquire({{{10, 8}}}, {recorded}, record, streams));
    open_requires = nullptr;
    assert(enabled("path8"));
    assert(!runtime.close(record));
    assert(!enabled("path8") && enabled("path4"));
    assert(!runtime.close(call));
    assert(open_pcms.empty());
}

static void joined_links()
{
    auto input = fixture();
    Json::Value selection;

    for (auto [key, value] : {std::pair{"0xa", "4"}, {"0xb", "1"}}) {
        Json::Value pair(Json::arrayValue);

        pair.append(key);
        pair.append(value);
        selection["graph_keys"].append(pair);
    }

    for (unsigned graph : {3, 4}) {
        Json::Value tuning;

        selection["graphs"].append(graph);
        tuning["graph_id"] = graph;
        tuning["subgraph"] = 0xb0000000u + graph;
        tuning["set_control"] = "set" + std::to_string(graph);
        tuning["modules"].append(0x4000 + graph);
        selection["tuning"].append(tuning);
    }

    selection["references"].append("link10");
    selection["references"].append("link11");
    input["selections"].append(selection);

    for (unsigned id : {10, 11}) {
        Json::Value reference;

        reference["name"] = "link" + std::to_string(id);
        reference["control"] = false;
        reference["source_graph"] = id == 10 ? 3 : 6;
        reference["destination_graph"] = 4;
        reference["path_control"] = "path" + std::to_string(id);
        input["references"].append(reference);
    }

    ar_topology::Bindings bindings(input);
    auto controls = make_mixer();

    for (const char *name : {"link10", "path10", "link11", "path11"})
        controls.controls[name];

    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId capture = 0;
    std::vector<ar_topology::Stream> streams;
    ar_topology::Keys echo{{11, 1}, {10, 4}};
    auto enabled = [&](const char *name) {
        return controls.controls.at(name).enabled;
    };

    assert(runtime.join(1, echo, true) == -ENOENT);
    assert(!runtime.acquire({{{10, 4}}}, {configuration(4)}, capture, streams));
    assert(runtime.join(capture, {{10, 9}}, true) == -ENOENT);
    assert(runtime.join(capture, {{10, 1}}, true) == -ENOENT);
    assert(!runtime.join(capture, echo, false));
    assert(!runtime.join(capture, echo, true));
    assert(enabled("link10") && !enabled("path10"));
    assert(!enabled("link11") && !enabled("hardware3"));
    assert(!runtime.join(capture, echo, true));
    assert(!runtime.join(capture, echo, false));
    assert(!enabled("link10") && enabled("link4"));

    controls.controls.at("link10").reject_enable = true;
    assert(runtime.join(capture, echo, true) == -EIO);
    controls.controls.at("link10").reject_enable = false;
    controls.controls.at("link10").enabled = 0;
    assert(!runtime.join(capture, echo, true));
    assert(enabled("link10"));
    assert(!runtime.close(capture));
    assert(!enabled("link10"));
    assert(open_pcms.empty());
}

static void stale_controls()
{
    ar_topology::Bindings bindings(fixture());
    auto controls = make_mixer();
    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId session = 0;
    std::vector<ar_topology::Stream> streams;

    for (const char *name : {"link1", "path1", "hardware3", "link6"})
        controls.controls.at(name).enabled = 1;

    assert(runtime.acquire({{{10, 1}}}, {configuration(1)}, session, streams)
           == -EBUSY);
    assert(!session && streams.empty() && open_pcms.empty());

    controls.controls.at("hardware3").reject_disable = true;
    assert(runtime.reset() == -EIO);
    controls.controls.at("hardware3").reject_disable = false;
    controls.controls.at("hardware3").enabled = 1;
    assert(controls.controls.at("link1").enabled);
    assert(!runtime.reset());

    for (const auto &[name, control] : controls.controls)
        assert(!control.enabled);

    assert(!runtime.acquire({{{10, 1}}}, {configuration(1)}, session, streams));
    assert(runtime.reset() == -EBUSY);
    assert(controls.controls.at("hardware3").enabled);
    assert(!runtime.close(session));

    auto link = controls.controls.at("link2");
    controls.controls.erase("link2");
    assert(runtime.reset() == -ENOENT);
    controls.controls["link2"] = link;
    assert(open_pcms.empty());
}

int main()
{
    stale_controls();
    joined_links();
    peer_paths();
    parameter_queries();
    reroutes();
    vcpm_parameters();
    calibration();
    media_configuration();
    ar_topology::Bindings bindings(fixture());
    auto controls = make_mixer();
    ar_topology::Runtime runtime(0, &controls, bindings);
    ar_topology::SessionId first = 0, second = 0, capture = 0;
    std::vector<ar_topology::Stream> a, b, c;
    Initialization init{controls, {1, 3}};
    assert(!runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                         initialize, &init));
    assert(a[0].handle->state == SNDRV_PCM_STATE_PREPARED);

    Bytes custom{0x10, 0x20};
    auto &device = controls.controls.at("set3");
    assert(!mixer_ctl_set_array(&device, custom.data(), custom.size()));
    init.defaults = {2};
    assert(!runtime.open({{{10, 2}}}, {configuration(2)}, second, b,
                         initialize, &init));
    assert(device.bytes == custom);
    init.defaults = {4, 5};
    assert(!runtime.open({{{10, 4}}}, {configuration(4)}, capture, c,
                         initialize, &init));
    assert(!runtime.close(first));
    assert(controls.controls.at("hardware3").enabled);
    assert(!controls.controls.at("path1").enabled);
    assert(controls.controls.at("path2").enabled);
    assert(b[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    assert(c[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    assert(device.bytes == custom);
    assert(!runtime.close(second));
    assert(!controls.controls.at("hardware3").enabled);
    assert(controls.controls.at("hardware5").enabled);
    assert(!runtime.close(capture));
    assert(open_pcms.empty());

    first = 0;
    a.clear();
    init.defaults = {1, 3};
    init.reject = true;
    prepared = false;
    assert(runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                        initialize, &init) == -ECANCELED);
    assert(!first && a.empty() && open_pcms.empty() && !prepared);
    assert(!controls.controls.at("hardware3").enabled);

    init.reject = false;
    controls.controls.at("hardware3").reject_enable = true;
    controls.controls.at("hardware3").reject_disable = true;
    assert(runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                        initialize, &init) == -EIO);
    assert(!first && a.empty() && open_pcms.empty());
    controls.controls.at("hardware3").reject_enable = false;
    controls.controls.at("hardware3").reject_disable = false;
    assert(!controls.controls.at("path1").enabled);

    first = 0;
    initialization_seen = false;
    assert(!runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                         initialize, &init));
    second = 0;
    b.clear();
    assert(runtime.open({{{10, 1}}}, {configuration(1)}, second, b) == -EBUSY);
    assert(runtime.open({{{10, 6}}}, {configuration(2)}, second, b) ==
           -EOPNOTSUPP);
    assert(!controls.controls.at("path6").enabled);
    assert(a[0].handle->state == SNDRV_PCM_STATE_PREPARED);

    rejected_open = 2;
    assert(runtime.acquire({{{10, 2}}}, {configuration(2)}, second, b) == -EIO);
    assert(!second && b.empty());
    assert(controls.controls.at("hardware3").enabled);
    rejected_open = -1;
    controls.controls.at("set2").reject_parameters = true;
    assert(runtime.open({{{10, 2}}}, {configuration(2)}, second, b,
                        initialize, &init) == -EIO);
    assert(!second && device.bytes.size() == 32);
    assert(controls.controls.at("hardware3").enabled);
    assert(!controls.controls.at("path2").enabled);
    controls.controls.at("set2").reject_parameters = false;
    init.defaults = {2};
    rejected_pcm = 2;
    assert(runtime.open({{{10, 2}}}, {configuration(2)}, second, b,
                        initialize, &init) == -EPIPE);
    assert(!second && b.empty());
    assert(a[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    assert(controls.controls.at("hardware3").enabled);
    rejected_pcm = -1;
    assert(!runtime.open({{{10, 2}}}, {configuration(2)}, second, b,
                         initialize, &init));
    controls.controls.at("path1").reject_disable = true;
    assert(runtime.close(first) == -EIO);
    assert(open_pcms.size() == 1);
    assert(b[0].handle->state == SNDRV_PCM_STATE_PREPARED);
    assert(controls.controls.at("hardware3").enabled);
    controls.controls.at("path1").reject_disable = false;
    assert(runtime.close(first) == -ENOENT);
    assert(!runtime.close(second));
    assert(open_pcms.empty());

    a.clear();
    first = 0;
    init.defaults = {1, 3, 4, 5};
    initialization_seen = false;
    assert(!runtime.open({{{10, 1}}, {{10, 4}}},
                         {configuration(1), configuration(4)}, first, a,
                         initialize, &init));
    assert(a.size() == 2);
    rejected_start = 4;
    assert(runtime.start(first) == -EIO);
    assert(a[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(a[1].handle->state == SNDRV_PCM_STATE_PREPARED);
    rejected_start = -1;
    assert(!runtime.start(first));
    assert(a[1].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(!runtime.stop(first));
    assert(!runtime.close(first));
    assert(open_pcms.empty());

    first = second = 0;
    a.clear();
    b.clear();
    prepared = false;
    assert(!runtime.acquire({{{10, 1}}}, {configuration(1)}, first, a));
    assert(!runtime.acquire({{{10, 2}}}, {configuration(2)}, second, b));
    assert(!prepared);
    assert(a[0].handle->state == SNDRV_PCM_STATE_SETUP);
    assert(b[0].handle->state == SNDRV_PCM_STATE_SETUP);
    int ring = -1;
    uint32_t ring_bytes = 0;
    std::vector<uint8_t> event{42};
    uint32_t registration[] = {0x4002, 0x08001000, 1, 0, 0, 0};
    assert(runtime.export_buffer(first, 2, ring, ring_bytes) == -ENOENT);
    assert(runtime.register_events(first, 2, registration,
        sizeof(registration)) == -ENOENT);
    assert(runtime.register_events(first, 1, registration,
        sizeof(registration)) == -ENOENT);
    assert(runtime.read_event(first, 2, event) == -ENOENT);
    int notifications = -1;
    assert(runtime.subscribe_events(first, 2, notifications) == -ENOENT);
    assert(notifications == -1);
    assert(!runtime.subscribe_events(first, 1, notifications));
    assert(fcntl(notifications, F_GETFD) & FD_CLOEXEC);
    close(notifications);
    notifications = -1;
    assert(ring == -1 && !ring_bytes && event == std::vector<uint8_t>{42});
    snd_pcm_sync_ptr sync{};
    sync.flags = SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL |
                 SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    int pcm_fd = -1;
    assert(runtime.sync_pcm(first, 2, sync) == -ENOENT);
    assert(runtime.pcm_poll_fd(first, 2, pcm_fd) == -ENOENT);
    assert(pcm_fd == -1);
    a[0].handle->position.s.status.hw_ptr = 127;
    a[0].handle->position.s.status.tstamp.tv_sec = 11;
    a[0].handle->position.s.status.tstamp.tv_nsec = 12000;
    b[0].handle->position.s.status.hw_ptr = 251;
    assert(!runtime.sync_pcm(first, 1, sync));
    assert(sync.s.status.state == SNDRV_PCM_STATE_SETUP);
    assert(sync.s.status.hw_ptr == 127);
    assert(sync.s.status.tstamp.tv_sec == 11 &&
           sync.s.status.tstamp.tv_nsec == 12000);
    assert(!runtime.sync_pcm(second, 2, sync));
    assert(sync.s.status.hw_ptr == 251);
    assert(!runtime.pcm_poll_fd(first, 1, pcm_fd));
    pollfd ready{pcm_fd, POLLOUT, 0};
    assert(poll(&ready, 1, 0) == 1 && (ready.revents & POLLOUT));
    sync.flags = SNDRV_PCM_SYNC_PTR_APPL;
    sync.c.control.avail_min = 48;
    assert(!runtime.sync_pcm(first, 1, sync));
    sync.flags = SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    sync.c.control.appl_ptr = 96;
    assert(!runtime.sync_pcm(first, 1, sync));
    sync.flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    sync.c.control.appl_ptr = sync.c.control.avail_min = 0;
    assert(!runtime.sync_pcm(first, 1, sync));
    assert(sync.c.control.appl_ptr == 96 && sync.c.control.avail_min == 48);
    const auto before = sync;
    a[0].handle->sync_error = EPIPE;
    assert(runtime.sync_pcm(first, 1, sync) == -EPIPE);
    assert(!std::memcmp(&sync, &before, sizeof(sync)));
    a[0].handle->sync_error = 0;
    a[0].handle->dup_error = ENOTTY;
    assert(runtime.export_buffer(first, 1, ring, ring_bytes) == -ENOTTY);
    assert(ring == -1 && !ring_bytes);
    a[0].handle->dup_error = 0;
    assert(!runtime.export_buffer(first, 1, ring, ring_bytes));
    assert(ring >= 0 && ring_bytes == 240 * 4 * 2 * 2);
    assert(runtime.export_buffer(first, 1, ring, ring_bytes) == -EINVAL);
    close(ring);
    ring = -1;
    registration[0] = 0x4001;
    assert(!runtime.register_events(first, 1, registration,
        sizeof(registration)));
    assert(!runtime.read_event(first, 1, event));
    uint32_t module = 0;
    assert(event.size() == 3 * sizeof(uint32_t));
    std::memcpy(&module, event.data(), sizeof(module));
    assert(module == 0x4001);
    assert(runtime.start(first) == -EBADFD);
    init.defaults = {1, 3};
    assert(!runtime.prepare(first, initialize, &init));
    assert(!mixer_ctl_set_array(&device, custom.data(), custom.size()));
    init.defaults = {2};
    assert(!runtime.prepare(second, initialize, &init));
    assert(device.bytes == custom);
    assert(!runtime.start(first));
    assert(!runtime.start(second));
    assert(!runtime.sync_pcm(first, 1, sync));
    assert(sync.s.status.hw_ptr == 127);
    assert(sync.s.status.state == SNDRV_PCM_STATE_RUNNING);
    rejected_stop = 1;
    assert(runtime.stop(first) == -EIO);
    assert(a[0].handle->state == SNDRV_PCM_STATE_SETUP);
    assert(b[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    rejected_stop = -1;
    assert(!runtime.stop(first));
    assert(a[0].handle->state == SNDRV_PCM_STATE_SETUP);
    assert(b[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(controls.controls.at("hardware3").enabled);
    assert(!runtime.prepare(first));
    assert(!runtime.start(first));
    assert(device.bytes == custom);
    assert(!runtime.stop(first));
    assert(!runtime.close(first));
    assert(b[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(!runtime.stop(second));
    assert(!runtime.close(second));
    assert(open_pcms.empty());

    first = 0;
    a.clear();
    init.defaults = {1, 3};
    assert(!runtime.open({{{10, 1}}}, {configuration(1)}, first, a,
                         initialize, &init));
    assert(!runtime.start(first));
    rejected_stop = 1;
    assert(runtime.close(first) == -EIO);
    rejected_stop = -1;
    assert(open_pcms.empty());
    assert(!controls.controls.at("hardware3").enabled);
    assert(runtime.close(first) == -ENOENT);
    assert(runtime.export_buffer(first, 1, ring, ring_bytes) == -ENOENT);
    assert(runtime.read_event(first, 1, event) == -ENOENT);
    assert(runtime.sync_pcm(first, 1, sync) == -ENOENT);
    int rejected_fd = -1;
    assert(runtime.pcm_poll_fd(first, 1, rejected_fd) == -ENOENT);

    auto compressed = fixture();
    compressed["frontends"][1]["mode"] = "compress";
    ar_topology::Bindings offload_bindings(compressed);
    ar_topology::Runtime offload_runtime(0, &controls, offload_bindings);
    ar_topology::SessionId offload = 0;
    std::vector<ar_topology::Stream> d;
    Initialization offload_init{controls, {2, 3}};
    assert(offload_runtime.open({{{10, 2}}}, {configuration(2)}, offload, d,
                                initialize, &offload_init) == -EINVAL);
    assert(d.empty() && open_pcms.empty());
    auto offload_config = configuration(2);
    snd_compr_params params{};
    params.buffer.fragment_size = 4096;
    params.buffer.fragments = 4;
    params.codec.id = SND_AUDIOCODEC_MP3;
    params.codec.sample_rate = 44100;
    offload_config.compress = params;
    compress_mixer = &controls;
    controls.controls.at("set2").bytes.clear();
    assert(!offload_runtime.acquire({{{10, 2}}}, {offload_config}, offload, d));
    assert(d.size() == 1 && !d[0].handle && d[0].compress == compress_fd);
    assert(compress_path == "/dev/snd/comprC0D2" && open_pcms.empty());
    assert(compress_access == O_WRONLY);
    assert(compress_commands.empty());
    assert(offload_runtime.start(offload) == -EBADFD);
    assert(!offload_runtime.prepare(offload, initialize, &offload_init));
    assert((compress_commands == std::vector<unsigned>{
        SNDRV_COMPRESS_SET_PARAMS}));
    assert(tuned_before_params);
    assert(compress_params.codec.id == SND_AUDIOCODEC_MP3 &&
           compress_params.buffer.fragments == 4);
    assert(!offload_runtime.prepare(offload) && compress_commands.size() == 1);
    assert(!offload_runtime.start(offload));
    int borrowed = -1;
    assert(!offload_runtime.compress_fd(offload, 2, borrowed));
    assert(borrowed == compress_fd);
    assert(offload_runtime.pcm_poll_fd(offload, 2, borrowed) == -EOPNOTSUPP);
    int exported = -1;
    uint32_t exported_bytes = 0;
    assert(offload_runtime.export_buffer(offload, 2, exported,
                                         exported_bytes) == -EOPNOTSUPP);
    assert(!offload_runtime.stop(offload));
    assert(compress_commands.back() == SNDRV_COMPRESS_STOP);
    int closing = compress_fd;
    assert(!offload_runtime.close(offload));
    assert(fcntl(closing, F_GETFD) == -1 && errno == EBADF);
    compress_fd = -1;
    d.clear();
    assert(runtime.compress_fd(first, 1, borrowed) == -ENOENT);

    auto encoded = fixture();
    encoded["frontends"][2]["mode"] = "compress";
    ar_topology::Bindings record_bindings(encoded);
    ar_topology::Runtime record_runtime(0, &controls, record_bindings);
    ar_topology::SessionId record = 0;
    auto record_config = configuration(4);
    params.codec.id = SND_AUDIOCODEC_AAC;
    record_config.compress = params;
    record_config.flags = PCM_OUT;
    assert(record_runtime.acquire({{{10, 4}}}, {record_config}, record, d) ==
           -EINVAL);
    record_config.flags = PCM_IN;
    compress_commands.clear();
    assert(!record_runtime.acquire({{{10, 4}}}, {record_config}, record, d));
    assert(compress_path == "/dev/snd/comprC0D4" && compress_access == O_RDONLY);
    assert(d.size() == 1 && d[0].compress == compress_fd && open_pcms.empty());
    assert(!record_runtime.prepare(record));
    assert(compress_params.codec.id == SND_AUDIOCODEC_AAC);
    assert(!record_runtime.start(record));
    assert(!record_runtime.close(record));
    compress_fd = -1;
    d.clear();

    auto hostless = fixture();
    hostless["frontends"][1]["mode"] = "hostless";
    ar_topology::Bindings hostless_bindings(hostless);
    ar_topology::Runtime hostless_runtime(0, &controls, hostless_bindings);
    ar_topology::SessionId silent = 0;
    auto silent_config = configuration(2);
    silent_config.config.avail_min = 60;
    assert(!hostless_runtime.open({{{10, 2}}}, {silent_config}, silent, d,
                                  initialize, &offload_init));
    assert(!d[0].handle->stop_threshold);
    assert(!hostless_runtime.start(silent));
    assert(d[0].handle->state == SNDRV_PCM_STATE_RUNNING);
    assert(d[0].handle->stop_threshold ==
           snd_pcm_uframes_t(d[0].handle->buffer_frames) << 20);
    assert(d[0].handle->avail_min == 60);
    assert(!hostless_runtime.stop(silent));
    assert(!hostless_runtime.close(silent));
    d.clear();
    assert(open_pcms.empty());

    ar_topology::SessionId hosted = 0;
    assert(!runtime.open({{{10, 2}}}, {configuration(2)}, hosted, d,
                         initialize, &offload_init));
    assert(!runtime.start(hosted));
    assert(!d[0].handle->stop_threshold);
    assert(!runtime.stop(hosted));
    assert(!runtime.close(hosted));
    d.clear();
    assert(open_pcms.empty());

    auto duplex = fixture();
    duplex["frontends"][1]["mode"] = "hostless";
    duplex["frontends"].append(duplex["frontends"][1]);
    duplex["frontends"][3]["direction"] = "capture";
    ar_topology::Bindings loopback_bindings(duplex);
    ar_topology::Runtime loopback_runtime(0, &controls, loopback_bindings);
    ar_topology::SessionId loopback = 0;
    auto capture_config = configuration(2);
    capture_config.flags = PCM_IN;
    assert(loopback_runtime.open({{{10, 2}}}, {configuration(2), configuration(2)},
                                 loopback, d, initialize, &offload_init) ==
           -EINVAL);
    assert(d.empty() && !loopback && open_pcms.empty());
    assert(!loopback_runtime.open({{{10, 2}}}, {capture_config, configuration(2)},
                                  loopback, d, initialize, &offload_init));
    assert(d.size() == 2 && d[0].id == 2 && d[1].id == 2);
    assert(!(d[0].handle->flags & PCM_IN) && (d[1].handle->flags & PCM_IN));
    assert(!loopback_runtime.start(loopback));

    for (const auto &stream : d) {
        assert(stream.handle->state == SNDRV_PCM_STATE_RUNNING);
        assert(stream.handle->stop_threshold ==
               snd_pcm_uframes_t(stream.handle->buffer_frames) << 20);
    }

    assert(loopback_runtime.sync_pcm(loopback, 2, sync) == -EOPNOTSUPP);
    assert(loopback_runtime.read_event(loopback, 2, event) == -EOPNOTSUPP);
    assert(!loopback_runtime.stop(loopback));
    assert(d[0].handle->state == SNDRV_PCM_STATE_SETUP &&
           d[1].handle->state == SNDRV_PCM_STATE_SETUP);
    assert(!loopback_runtime.close(loopback));
    assert(open_pcms.empty());
}
