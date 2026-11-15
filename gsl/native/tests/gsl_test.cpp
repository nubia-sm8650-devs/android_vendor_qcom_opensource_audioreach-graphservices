/* SPDX-License-Identifier: BSD-3-Clause */
#include "fake_card.h"

#include "gsl_native.h"

#include <atomic>
#include <fstream>
#include <gsl_intf.h>

static mixer *card_mixer;
static int open_mixers;
static int absent_scans;
static int acdb_users;
static int32_t acdb_error;
static std::atomic<bool> card_lost{false};

extern "C" struct mixer *mixer_open(unsigned card)
{
    if (card || !card_mixer)
        return nullptr;

    if (absent_scans) {
        absent_scans--;

        return nullptr;
    }

    open_mixers++;

    return card_mixer;
}

extern "C" const char *mixer_get_name(struct mixer *)
{
    return "CARD";
}

extern "C" void mixer_close(struct mixer *)
{
    open_mixers--;
}

extern "C" int32_t acdb_init(AcdbDataFiles *files, AcdbFile *)
{
    assert(files);

    if (acdb_error)
        return acdb_error;

    acdb_users++;

    return 0;
}

extern "C" int32_t acdb_deinit(void)
{
    acdb_users--;

    return 0;
}

static struct pcm *opened(unsigned id)
{
    for (auto *handle : open_pcms) {
        if (handle->id == id)
            return handle;
    }

    return nullptr;
}

static gsl_handle_t open_graph(uint32_t selection)
{
    gsl_key_value_pair pair{0xa, selection};
    gsl_key_vector keys{1, &pair};
    gsl_handle_t handle = nullptr;

    assert(!gsl_open(&keys, nullptr, &handle) && handle);

    return handle;
}

static void blocking_pcm()
{
    auto controls = make_mixer();

    initialization_seen = true;

    gsl_key_value_pair pair{0xa, 1};
    gsl_key_vector keys{1, &pair};
    gsl_handle_t handle = nullptr;

    assert(gsl_open(&keys, nullptr, &handle) == AR_ENOTREADY);
    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(fixture())));
    assert(gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(fixture())) == -EALREADY);

    pair.value = 9;
    assert(gsl_open(&keys, nullptr, &handle));
    assert(gsl_ioctl(&pair, GSL_CMD_PREPARE, nullptr, 0) == AR_EHANDLE);

    auto playback = open_graph(1);
    gsl_cmd_configure_pcm_media media{48000, 2, 16, 16};
    gsl_cmd_configure_read_write_params buffers{};

    buffers.buff_size = 960;
    buffers.num_buffs = 4;
    buffers.attributes = GSL_DATA_MODE_BLOCKING;

    assert(gsl_ioctl(playback, GSL_CMD_PREPARE, nullptr, 0) == AR_ENOTREADY);
    assert(open_pcms.empty());
    assert(!gsl_ioctl(playback, GSL_CMD_CONFIGURE_PCM_MEDIA, &media,
                      sizeof(media)));
    assert(gsl_ioctl(playback, GSL_CMD_CONFIGURE_PCM_MEDIA, &media, 4) ==
           AR_EBADPARAM);
    assert(gsl_ioctl(playback, GSL_CMD_PREPARE, nullptr, 0) == AR_ENOTREADY);
    assert(!gsl_ioctl(playback, GSL_CMD_CONFIGURE_WRITE_PARAMS, &buffers,
                      sizeof(buffers)));
    assert(!gsl_ioctl(playback, GSL_CMD_PREPARE, nullptr, 0));

    auto *pcm = opened(1);

    assert(pcm && open_pcms.size() == 1 && !(pcm->flags & PCM_IN));
    assert(pcm->config.rate == 48000 && pcm->config.channels == 2 &&
           pcm->config.format == PCM_FORMAT_S16_LE &&
           pcm->config.period_size == 240 && pcm->config.period_count == 4);
    assert(pcm->state == SNDRV_PCM_STATE_PREPARED);
    assert(pcm->start_threshold == 961);
    assert(gsl_ioctl(playback, GSL_CMD_CONFIGURE_PCM_MEDIA, &media,
                     sizeof(media)) == AR_EBUSY);

    std::vector<uint8_t> data(960);
    gsl_buff buffer{};
    uint32_t moved = 0;

    buffer.addr = data.data();
    buffer.size = 962;
    assert(gsl_write(playback, 0, &buffer, &moved) == AR_EBADPARAM);
    buffer.size = 960;
    assert(!gsl_write(playback, 0, &buffer, &moved) && moved == 960);
    assert(pcm->moved == 240);
    assert(gsl_read(playback, 0, &buffer, &moved) == AR_EBADPARAM);
    assert(!gsl_ioctl(playback, GSL_CMD_START, nullptr, 0));
    assert(pcm->state == SNDRV_PCM_STATE_RUNNING);
    assert(!gsl_write(playback, 0, &buffer, &moved) && moved == 960);
    assert(pcm->moved == 480);

    auto capture = open_graph(4);

    media = {16000, 1, 24, 32};
    buffers.buff_size = 640;
    buffers.num_buffs = 2;
    assert(gsl_ioctl(capture, GSL_CMD_START, nullptr, 0) == AR_ENOTREADY);
    assert(!gsl_ioctl(capture, GSL_CMD_CONFIGURE_PCM_MEDIA, &media,
                      sizeof(media)));
    assert(!gsl_ioctl(capture, GSL_CMD_CONFIGURE_READ_PARAMS, &buffers,
                      sizeof(buffers)));
    assert(!gsl_ioctl(capture, GSL_CMD_PREPARE, nullptr, 0));

    auto *input = opened(4);

    assert(input && open_pcms.size() == 2 && (input->flags & PCM_IN));
    assert(input->config.rate == 16000 && input->config.channels == 1 &&
           input->config.format == PCM_FORMAT_S24_LE &&
           input->config.period_size == 160);

    assert(!gsl_ioctl(playback, GSL_CMD_STOP, nullptr, 0));
    assert(pcm->state == SNDRV_PCM_STATE_SETUP);
    assert(!gsl_close(playback));
    assert(gsl_close(playback) == AR_EHANDLE);
    assert(open_pcms.size() == 1);

    gsl_native::stop();
    assert(open_pcms.empty());
    assert(gsl_close(capture) == AR_ENOTREADY);

    for (const auto &[name, control] : controls.controls)
        assert(!control.enabled);
}

static void configure(gsl_handle_t handle, bool capture)
{
    gsl_cmd_configure_pcm_media media{48000, 2, 16, 16};
    gsl_cmd_configure_read_write_params buffers{};

    buffers.buff_size = 960;
    buffers.num_buffs = 4;
    buffers.attributes = GSL_DATA_MODE_BLOCKING;

    assert(!gsl_ioctl(handle, GSL_CMD_CONFIGURE_PCM_MEDIA, &media,
                      sizeof(media)));
    assert(!gsl_ioctl(handle, capture ? GSL_CMD_CONFIGURE_READ_PARAMS :
                                        GSL_CMD_CONFIGURE_WRITE_PARAMS,
                      &buffers, sizeof(buffers)));
}

static void restarts()
{
    auto controls = make_mixer();

    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(fixture())));

    auto playback = open_graph(1);

    configure(playback, false);
    assert(!gsl_ioctl(playback, GSL_CMD_PREPARE, nullptr, 0));

    auto *pcm = opened(1);

    assert(!gsl_ioctl(playback, GSL_CMD_START, nullptr, 0));
    assert(pcm->state == SNDRV_PCM_STATE_RUNNING);

    gsl_native::stop();
    assert(open_pcms.empty());
}

static void parameters()
{
    auto input = fixture();

    for (auto &selection : input["selections"]) {
        for (auto &tuning : selection["tuning"]) {
            if (tuning["graph_id"].asUInt() == 1)
                tuning["get_control"] = "get1";
        }
    }

    auto controls = make_mixer();
    auto &get = controls.controls["get1"];
    auto &set = controls.controls.at("set1");

    get.parameter = true;
    get.reply = {1, 2, 3, 4};
    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(input)));

    gsl_key_value_pair selection{0xa, 1}, first{50, 5}, second{50, 6};
    gsl_key_vector keys{1, &selection}, ckv{1, &first};
    gsl_handle_t handle = nullptr;

    assert(!gsl_open(&keys, &ckv, &handle));
    configure(handle, false);

    uint32_t record[] = {0x4001, 0x08001234, 4, 0, 77, 0};
    uint32_t query[] = {0x4001, 0x0800113d, 4, 0, 0, 0};
    auto *bytes = reinterpret_cast<uint8_t *>(record);

    assert(gsl_set_custom_config(handle, nullptr, 8) == AR_EBADPARAM);
    assert(!gsl_set_custom_config(handle, bytes, sizeof(record)));
    assert(set.bytes.empty());
    assert(gsl_get_custom_config(handle,
        reinterpret_cast<uint8_t *>(query), sizeof(query)) == AR_ENOTREADY);

    ckv.kvp = &second;
    selection.value = 2;
    assert(gsl_set_cal(handle, &keys, &ckv) == AR_EBADPARAM);
    selection.value = 1;
    assert(!gsl_set_cal(handle, &keys, &ckv));

    calibrated.clear();
    assert(!gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0));
    assert(calibrated.at(0xb0000001).second == ar_topology::Keys({{50, 6}}));
    assert(set.bytes.size() == 8 + sizeof(record));
    assert(!std::memcmp(set.bytes.data() + 8, record, sizeof(record)));

    record[4] = 78;
    assert(!gsl_set_custom_config(handle, bytes, sizeof(record)));
    assert(!std::memcmp(set.bytes.data() + 8, record, sizeof(record)));

    assert(!gsl_get_custom_config(handle,
        reinterpret_cast<uint8_t *>(query), sizeof(query)));
    assert(query[4] == 0x04030201);

    calibrated.clear();
    ckv.kvp = &first;
    assert(!gsl_set_cal(handle, nullptr, &ckv));
    assert(calibrated.at(0xb0000001).first == ar_topology::Keys({{50, 6}}) &&
           calibrated.at(0xb0000001).second == ar_topology::Keys({{50, 5}}));

    gsl_native::stop();
    assert(open_pcms.empty());
}

static void hostless()
{
    auto input = fixture();

    for (auto &frontend : input["frontends"]) {
        if (frontend["pcm_id"].asUInt() == 4)
            frontend["mode"] = "hostless";
    }

    auto controls = make_mixer();

    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(input)));

    auto handle = open_graph(4);
    gsl_cmd_configure_pcm_media media{16000, 1, 16, 16};

    assert(gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0) == AR_ENOTREADY);
    assert(!gsl_ioctl(handle, GSL_CMD_CONFIGURE_PCM_MEDIA, &media,
                      sizeof(media)));
    assert(!gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0));

    auto *pcm = opened(4);

    assert(pcm && pcm->config.rate == 16000 && pcm->config.channels == 1 &&
           pcm->config.period_size == 160 && pcm->config.period_count == 2);
    assert(!gsl_ioctl(handle, GSL_CMD_START, nullptr, 0));
    assert(pcm->state == SNDRV_PCM_STATE_RUNNING);

    std::vector<uint8_t> data(320);
    gsl_buff buffer{};
    uint32_t moved = 0;

    buffer.addr = data.data();
    buffer.size = data.size();
    assert(gsl_read(handle, 0, &buffer, &moved) == AR_EUNSUPPORTED);
    assert(!pcm->moved);

    gsl_native::stop();
    assert(open_pcms.empty());
}

static void mapped()
{
    auto controls = make_mixer();

    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(fixture())));

    auto handle = open_graph(1);
    gsl_cmd_configure_pcm_media media{48000, 2, 16, 16};
    gsl_cmd_configure_read_write_params buffers{};
    gsl_shmem_buf entry{};
    gsl_cmd_get_shmem_buf_info info{0, 1, &entry};

    buffers.buff_size = 960;
    buffers.num_buffs = 4;
    buffers.attributes = GSL_DATA_MODE_PUSH_PULL;

    assert(gsl_ioctl(handle, GSL_CMD_GET_WRITE_BUFF_INFO, &info,
                     sizeof(info)) == AR_ENOTREADY);
    assert(!gsl_ioctl(handle, GSL_CMD_CONFIGURE_PCM_MEDIA, &media,
                      sizeof(media)));
    assert(!gsl_ioctl(handle, GSL_CMD_CONFIGURE_WRITE_PARAMS, &buffers,
                      sizeof(buffers)));
    assert(open_pcms.empty());
    assert(!gsl_ioctl(handle, GSL_CMD_GET_WRITE_BUFF_INFO, &info,
                      sizeof(info)));

    auto *pcm = opened(1);

    assert(pcm && (pcm->flags & PCM_MMAP) && (pcm->flags & PCM_NOIRQ));
    assert(pcm->config.period_size == 240 && pcm->config.period_count == 4);
    assert(pcm->stop_threshold == snd_pcm_uframes_t(960) << 20);
    assert(info.size == 3840 && entry.metadata);

    int data = *reinterpret_cast<int32_t *>(uintptr_t(entry.metadata));

    assert(fcntl(data, F_GETFD) >= 0);
    entry = {};
    assert(!gsl_ioctl(handle, GSL_CMD_GET_WRITE_BUFF_INFO, &info,
                      sizeof(info)));
    assert(*reinterpret_cast<int32_t *>(uintptr_t(entry.metadata)) == data);

    entry = {};
    assert(!gsl_ioctl(handle, GSL_CMD_GET_WRITE_POS_BUFF_INFO, &info,
                      sizeof(info)));
    assert(!info.size);
    assert(*reinterpret_cast<int32_t *>(uintptr_t(entry.metadata)) ==
           pcm_get_poll_fd(pcm));
    assert(gsl_ioctl(handle, GSL_CMD_GET_READ_BUFF_INFO, &info,
                     sizeof(info)) == AR_EUNSUPPORTED);

    assert(!gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0));
    assert(!gsl_ioctl(handle, GSL_CMD_START, nullptr, 0));
    assert(pcm->state == SNDRV_PCM_STATE_RUNNING);

    std::vector<uint8_t> bytes(960);
    gsl_buff buffer{};
    uint32_t moved = 0;

    buffer.addr = bytes.data();
    buffer.size = bytes.size();
    assert(gsl_write(handle, 0, &buffer, &moved) == AR_EUNSUPPORTED);

    assert(!gsl_close(handle));
    assert(fcntl(data, F_GETFD) < 0 && open_pcms.empty());
    gsl_native::stop();
}

static void backend_media()
{
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
    backend["aif"]["id"] = 0;
    backend["aif"]["name"] = "SPEAKER-RX";

    for (const char *key : {"rate", "format", "channels", "data_format"})
        backend["controls"][key] = std::string("backend ") + key;

    auto controls = make_mixer();

    controls.controls["backend rate"].enums = {"96000", "48000"};
    controls.controls["backend channels"].enums = {"1", "2"};
    controls.controls["backend format"].enums = {"S32_LE", "S16_LE"};
    controls.controls["backend data_format"].enums = {
        "Topology", "Compressed over PCM", "Fixed Point"};

    uint32_t count = 0;

    assert(gsl_get_backends(nullptr, &count) == AR_ENOTREADY);
    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(input)));
    assert(!gsl_get_backends(nullptr, &count) && count == 1);

    gsl_backend listed[1]{};

    count = 0;
    assert(gsl_get_backends(listed, &count) == AR_ENEEDMORE && count == 1);
    assert(!gsl_get_backends(listed, &count));
    assert(!listed[0].id && !listed[0].is_capture &&
           std::string(listed[0].name) == "SPEAKER-RX");

    auto handle = open_graph(1);
    gsl_key_value_pair pair{0xa, 1};
    gsl_cmd_configure_backend_media media{{1, &pair}, 0, 48000, 2, 16, 16, 1};

    configure(handle, false);
    media.backend = 7;
    assert(gsl_ioctl(handle, GSL_CMD_CONFIGURE_BACKEND_MEDIA, &media,
                     sizeof(media)) == AR_ENOTEXIST);
    media.backend = 0;
    media.bit_width = 20;
    assert(gsl_ioctl(handle, GSL_CMD_CONFIGURE_BACKEND_MEDIA, &media,
                     sizeof(media)) == AR_EUNSUPPORTED);
    media.bit_width = 16;
    assert(!gsl_ioctl(handle, GSL_CMD_CONFIGURE_BACKEND_MEDIA, &media,
                      sizeof(media)));

    backend_mixer = &controls;
    backend_rate = 48000;
    backend_data_format = "Fixed Point";
    assert(!gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0));
    assert(controls.controls.at("backend rate").enabled == 1 &&
           controls.controls.at("backend format").enabled == 1 &&
           controls.controls.at("backend channels").enabled == 1 &&
           controls.controls.at("backend data_format").enabled == 2);

    backend_mixer = nullptr;
    gsl_native::stop();
    assert(open_pcms.empty());
}

static Json::Value loopback_input()
{
    auto input = fixture();
    Json::Value device, merged, pair(Json::arrayValue);

    pair.append("0xb");
    pair.append("1");
    device["graph_keys"].append(pair);
    device["graphs"].append(5);

    Json::Value tuning;

    tuning["graph_id"] = 5;
    tuning["subgraph"] = 0xb0000005u;
    tuning["set_control"] = "set5";
    tuning["modules"].append(0x4005);
    device["tuning"].append(tuning);
    device["references"] = Json::Value(Json::arrayValue);
    input["selections"].append(device);

    Json::Value reference;

    reference["name"] = "link5";
    reference["control"] = false;
    reference["source_graph"] = 3;
    reference["destination_graph"] = 5;
    reference["path_control"] = "path5";
    input["references"].append(reference);

    merged = input["selections"][0];
    merged["graph_keys"].append(pair);
    merged["graphs"].append(5);
    merged["tuning"].append(tuning);
    merged["references"].append("link5");
    input["selections"].append(merged);

    return input;
}

static void passive()
{
    auto controls = make_mixer();

    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(loopback_input())));

    gsl_key_value_pair pair{0xb, 1};
    gsl_key_vector keys{1, &pair};
    gsl_handle_t handle = nullptr;

    assert(!gsl_open(&keys, nullptr, &handle));
    assert(!gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0));
    assert(!gsl_ioctl(handle, GSL_CMD_START, nullptr, 0));
    assert(open_pcms.empty() && !controls.controls.at("hardware5").enabled);
    assert(!gsl_ioctl(handle, GSL_CMD_STOP, nullptr, 0));
    assert(!gsl_close(handle));

    gsl_native::stop();
}

static void loopback()
{
    auto controls = make_mixer();

    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(loopback_input())));

    auto playback = open_graph(1);

    configure(playback, false);
    assert(!gsl_ioctl(playback, GSL_CMD_PREPARE, nullptr, 0));

    gsl_key_value_pair device{0xb, 1}, both[] = {{0xa, 1}, {0xb, 1}};
    gsl_key_vector keys{1, &device};
    gsl_cmd_graph_select selection{{2, both}, {0, nullptr}};
    gsl_cmd_remove_graph removal{{2, both}};
    gsl_handle_t capture = nullptr;
    auto &gate = controls.controls.at("hardware5");
    auto &set = controls.controls.at("set5");

    assert(!gsl_open(&keys, nullptr, &capture));
    assert(!gsl_ioctl(capture, GSL_CMD_PREPARE, nullptr, 0));

    uint32_t record[] = {0x4005, 0x08001234, 4, 0, 77, 0};
    auto *bytes = reinterpret_cast<uint8_t *>(record);

    assert(!gsl_set_custom_config(capture, bytes, sizeof(record)));
    assert(!gate.enabled);
    set.bytes.clear();

    assert(!gsl_ioctl(capture, GSL_CMD_ADD_GRAPH, &selection,
                      sizeof(selection)));
    assert(gate.enabled && controls.controls.at("hardware3").enabled);
    assert(open_pcms.size() == 1 && opened(1));
    assert(set.bytes.size() == 8 + sizeof(record) &&
           !std::memcmp(set.bytes.data() + 8, record, sizeof(record)));

    record[4] = 78;
    assert(!gsl_set_custom_config(capture, bytes, sizeof(record)));
    assert(!std::memcmp(set.bytes.data() + 8, record, sizeof(record)));

    assert(!gsl_ioctl(capture, GSL_CMD_REMOVE_GRAPH, &removal,
                      sizeof(removal)));
    assert(!gate.enabled && controls.controls.at("hardware3").enabled);
    assert(gsl_ioctl(capture, GSL_CMD_REMOVE_GRAPH, &removal,
                     sizeof(removal)) == AR_ENOTEXIST);

    assert(!gsl_ioctl(capture, GSL_CMD_ADD_GRAPH, &selection,
                      sizeof(selection)));
    assert(gate.enabled);
    assert(!gsl_close(capture));
    assert(!gate.enabled && opened(1));

    assert(!gsl_open(&keys, nullptr, &capture));
    assert(!gsl_ioctl(capture, GSL_CMD_ADD_GRAPH, &selection,
                      sizeof(selection)));
    assert(gate.enabled);
    assert(!gsl_close(playback));
    assert(!gate.enabled && open_pcms.empty());

    record[4] = 79;
    set.bytes.clear();
    assert(!gsl_set_custom_config(capture, bytes, sizeof(record)));
    assert(set.bytes.empty());

    playback = open_graph(1);
    configure(playback, false);
    assert(!gsl_ioctl(playback, GSL_CMD_PREPARE, nullptr, 0));
    assert(gate.enabled && opened(1));
    assert(set.bytes.size() == 8 + sizeof(record) &&
           !std::memcmp(set.bytes.data() + 8, record, sizeof(record)));
    assert(!gsl_close(capture));
    assert(!gate.enabled && opened(1));

    assert(!gsl_close(playback));
    playback = open_graph(1);
    assert(!gsl_open(&keys, nullptr, &capture));
    assert(!gsl_ioctl(capture, GSL_CMD_ADD_GRAPH, &selection,
                      sizeof(selection)));
    assert(open_pcms.empty() && !gate.enabled);
    configure(playback, false);
    assert(!gsl_ioctl(playback, GSL_CMD_PREPARE, nullptr, 0));
    assert(gate.enabled);
    assert(!gsl_ioctl(capture, GSL_CMD_REMOVE_GRAPH, &removal,
                      sizeof(removal)));
    assert(!gate.enabled && opened(1));

    gsl_native::stop();
    assert(open_pcms.empty());
}

static void routes()
{
    auto controls = make_mixer();

    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(fixture())));

    auto handle = open_graph(2);
    gsl_key_value_pair pair{0xa, 6}, ckv{50, 5};
    gsl_cmd_graph_select selection{{1, &pair}, {1, &ckv}};
    gsl_cmd_remove_graph removal{{1, &pair}};
    auto &gate = controls.controls.at("hardware6");

    pair.value = 9;
    assert(gsl_ioctl(handle, GSL_CMD_CHANGE_GRAPH, &selection,
                     sizeof(selection)));
    pair.value = 6;
    assert(!gsl_ioctl(handle, GSL_CMD_CHANGE_GRAPH, &selection,
                      sizeof(selection)));
    configure(handle, false);
    calibrated.clear();
    assert(!gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0));
    assert(gate.enabled && opened(2) && open_pcms.size() == 1);
    assert(calibrated.at(0xb0000006).second == ar_topology::Keys({{50, 5}}));

    pair.value = 2;
    assert(!gsl_ioctl(handle, GSL_CMD_CHANGE_GRAPH, &selection,
                      sizeof(selection)));
    assert(!gate.enabled && opened(2));

    pair.value = 6;
    assert(!gsl_ioctl(handle, GSL_CMD_ADD_GRAPH, &selection,
                      sizeof(selection)));
    assert(gsl_ioctl(handle, GSL_CMD_ADD_GRAPH, &selection,
                     sizeof(selection)) == AR_EALREADY);
    assert(gate.enabled);

    gsl_cmd_properties properties{};

    assert(!gsl_ioctl(handle, GSL_CMD_STOP, &properties, sizeof(properties)));
    assert(!gsl_ioctl(handle, GSL_CMD_CLOSE_WITH_PROPS, &properties,
                      sizeof(properties)));
    assert(gate.enabled && opened(2)->state == SNDRV_PCM_STATE_PREPARED);

    assert(!gsl_ioctl(handle, GSL_CMD_REMOVE_GRAPH, &removal,
                      sizeof(removal)));
    assert(gsl_ioctl(handle, GSL_CMD_REMOVE_GRAPH, &removal,
                     sizeof(removal)) == AR_ENOTEXIST);
    assert(!gate.enabled && controls.controls.at("hardware3").enabled);

    pair.value = 2;
    assert(gsl_ioctl(handle, GSL_CMD_REMOVE_GRAPH, &removal,
                     sizeof(removal)) == AR_EBADPARAM);
    pair.value = 1;
    assert(gsl_ioctl(handle, GSL_CMD_CHANGE_GRAPH, &selection,
                     sizeof(selection)));
    assert(opened(2) && !opened(1));

    gsl_native::stop();
    assert(open_pcms.empty());
}

struct Received {
    std::atomic<int> count{0};
    uint32_t module = 0;
    uint32_t event = 0;
    uint32_t size = 0;
};

static void receive(gsl_event_cb_params *event, void *client)
{
    auto &received = *static_cast<Received *>(client);

    received.module = event->source_module_id;
    received.event = event->event_id;
    received.size = event->event_payload_size;
    received.count++;
}

static void events()
{
    auto controls = make_mixer();

    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(fixture())));

    auto handle = open_graph(1);
    Received received;
    gsl_cmd_register_custom_event request{0x4001, 0x08001000, 0, 1};

    configure(handle, false);
    registered_events = 0;
    queued_events = 0;
    assert(!gsl_register_event_cb(handle, receive, &received));
    assert(gsl_ioctl(handle, GSL_CMD_REGISTER_CUSTOM_EVENT, &request, 4) ==
           AR_EBADPARAM);
    assert(!gsl_ioctl(handle, GSL_CMD_REGISTER_CUSTOM_EVENT, &request,
                      sizeof(request)));
    assert(!registered_events && notification_ends.empty());
    assert(!gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0));
    assert(registered_events == 1 && notification_ends.size() == 1);

    request.module_instance_id = 0x4002;
    assert(gsl_ioctl(handle, GSL_CMD_REGISTER_CUSTOM_EVENT, &request,
                     sizeof(request)) == AR_ENOTEXIST);
    assert(registered_events == 1);

    snd_ctl_event notification{};

    queued_events = 1;
    assert(write(notification_ends[0].second, &notification,
                 sizeof(notification)) == sizeof(notification));

    for (int i = 0; i < 500 && !received.count; i++)
        usleep(10000);

    assert(received.count == 1 && received.module == 0x4001 &&
           received.event == 0x08001000 && !received.size);

    assert(write(notification_ends[0].second, &notification,
                 sizeof(notification)) == sizeof(notification));
    usleep(50000);
    assert(received.count == 1);

    assert(!gsl_close(handle));
    close(notification_ends[0].second);
    notification_ends.clear();
    queued_events = -1;
    gsl_native::stop();
    assert(open_pcms.empty());
}

static void count(gsl_event_cb_params *event, void *client)
{
    auto &counts = *static_cast<std::atomic<int> *>(client);

    assert(!event->event_payload_size);

    if (event->event_id == 0x08001126) {
        assert(event->source_module_id != GSL_EVENT_SRC_MODULE_ID_GSL);
        assert(counts >= 100);
        counts += 10000;

        return;
    }

    assert(event->source_module_id == GSL_EVENT_SRC_MODULE_ID_GSL);

    if (event->event_id == GSL_EVENT_ID_WRITE_DONE)
        counts += 1;
    else if (event->event_id == GSL_EVENT_ID_EOS)
        counts += 100;
}

static bool issued(unsigned long command)
{
    return std::find(compress_commands.begin(), compress_commands.end(),
                     command) != compress_commands.end();
}

static void compressed()
{
    auto input = fixture();

    input["frontends"][1]["mode"] = "compress";

    auto controls = make_mixer();

    compress_mixer = &controls;
    compress_commands.clear();
    assert(!gsl_native::start(0, &controls,
        std::make_unique<ar_topology::Bindings>(input)));

    auto handle = open_graph(2);
    std::atomic<int> counts{0};
    snd_compr_params params{};

    params.codec.id = SND_AUDIOCODEC_MP3;
    params.codec.sample_rate = 44100;
    params.codec.ch_in = params.codec.ch_out = 2;
    params.buffer.fragment_size = 1024;
    params.buffer.fragments = 4;

    assert(!gsl_register_event_cb(handle, count, &counts));
    assert(gsl_ioctl(handle, GSL_CMD_EOS, nullptr, 0) == AR_EUNSUPPORTED);
    assert(gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0) == AR_ENOTREADY);
    assert(gsl_ioctl(handle, GSL_CMD_CONFIGURE_COMPRESS_MEDIA, &params, 8) ==
           AR_EBADPARAM);
    assert(!gsl_ioctl(handle, GSL_CMD_CONFIGURE_COMPRESS_MEDIA, &params,
                      sizeof(params)));
    assert(!gsl_ioctl(handle, GSL_CMD_PREPARE, nullptr, 0));
    assert(compress_path == "/dev/snd/comprC0D2" && open_pcms.empty());
    assert(compress_params.codec.id == SND_AUDIOCODEC_MP3 &&
           compress_params.buffer.fragment_size == 1024);
    uint32_t trim[] = {0x4002, 0x0800115d, 4, 0, 576, 0};

    assert(!gsl_set_custom_config(handle,
        reinterpret_cast<uint8_t *>(trim), sizeof(trim)));
    assert(compress_metadata.key == SNDRV_COMPRESS_ENCODER_PADDING &&
           compress_metadata.value[0] == 576);
    trim[1] = 0x0800114b;
    trim[4] = 1105;
    assert(!gsl_set_custom_config(handle,
        reinterpret_cast<uint8_t *>(trim), sizeof(trim)));
    assert(compress_metadata.key == SNDRV_COMPRESS_ENCODER_DELAY &&
           compress_metadata.value[0] == 1105);
    assert(controls.controls.at("set2").bytes.empty() ||
           std::memcmp(controls.controls.at("set2").bytes.data() + 8, trim,
                       8));

    assert(!issued(SNDRV_COMPRESS_START));
    assert(!gsl_ioctl(handle, GSL_CMD_START, nullptr, 0));
    assert(issued(SNDRV_COMPRESS_START));

    std::vector<uint8_t> data(2048, 7), echo(2048);
    gsl_buff buffer{};
    uint32_t moved = 0;

    buffer.addr = data.data();
    buffer.size = data.size();
    assert(gsl_read(handle, 0, &buffer, &moved) == AR_EUNSUPPORTED);
    assert(!gsl_write(handle, 0, &buffer, &moved) && moved == 2048);
    assert(read(compress_peer, echo.data(), echo.size()) == 2048 &&
           echo == data);
    usleep(50000);
    assert(!counts);

    assert(!gsl_ioctl(handle, GSL_CMD_EOS, nullptr, 0));

    for (int i = 0; i < 500 && counts < 10102; i++)
        usleep(10000);

    assert(counts == 10102 && issued(SNDRV_COMPRESS_PARTIAL_DRAIN));

    compress_commands.clear();
    assert(!gsl_ioctl(handle, GSL_CMD_FLUSH, nullptr, 0));
    assert(issued(SNDRV_COMPRESS_STOP));
    assert(!gsl_ioctl(handle, GSL_CMD_STOP, nullptr, 0));
    assert(!gsl_close(handle));
    assert(fcntl(compress_fd, F_GETFD) < 0);

    close(compress_peer);
    compress_fd = compress_peer = -1;
    compress_mixer = nullptr;
    gsl_native::stop();
}

static int32_t database(uint32_t command, const void *input, void *output)
{
    static const uint32_t graph[] = {0xb0000001, 1, 0xb0000003, 0xb0000003, 0};

    if (command == ACDB_CMD_GET_GRAPH) {
        auto &keys = *static_cast<const AcdbGraphKeyVector *>(input);
        auto &reply = *static_cast<AcdbGetGraphRsp *>(output);

        assert(keys.num_keys == 1 && keys.graph_key_vector[0].value == 1);
        reply.num_subgraphs = 2;
        reply.size = sizeof(graph);

        if (reply.subgraphs)
            std::memcpy(reply.subgraphs, graph, sizeof(graph));

        return AR_EOK;
    }

    if (command == ACDB_CMD_GET_TAGGED_MODULES) {
        auto &request = *static_cast<const AcdbGetTaggedModulesReq *>(input);
        auto &reply = *static_cast<AcdbGetTaggedModulesRsp *>(output);

        assert(request.num_sg_ids == 2 && request.sg_ids[0] == 0xb0000001 &&
               request.sg_ids[1] == 0xb0000003);

        if (request.tag_id != 5)
            return AR_ENOTEXIST;

        reply.num_tagged_mids = 1;

        if (reply.tagged_mid_list)
            reply.tagged_mid_list[0] = {0x7001, 0x4001};

        return AR_EOK;
    }

    if (command == ACDB_CMD_GET_MODULE_TAG_DATA) {
        auto &request = *static_cast<const AcdbSgIdModuleTag *>(input);
        auto &reply = *static_cast<AcdbBlob *>(output);

        assert(request.num_sg_ids == 2 && request.module_tag.tag_id == 5 &&
               request.module_tag.tag_key_vector.num_keys == 1);
        reply.buf_size = 4;

        if (reply.buf)
            std::memcpy(reply.buf, "data", 4);

        return AR_EOK;
    }

    assert(command == ACDB_CMD_GET_TAGS_FROM_GKV);

    auto &reply = *static_cast<AcdbCmdGetTagsFromGkvRsp *>(output);

    reply.num_tags = 3;
    reply.list_size = 24;

    return AR_EOK;
}

static void queries()
{
    gsl_key_value_pair selection{0xa, 1}, value{30, 3};
    gsl_key_vector keys{1, &selection}, tkv{1, &value};
    gsl_module_id_info *modules = nullptr;
    uint32_t size = 0;

    acdb_query = database;

    assert(gsl_get_tagged_module_info(&keys, 6, &modules, &size) ==
           AR_ENOTEXIST);
    assert(!modules && !size);
    assert(!gsl_get_tagged_module_info(&keys, 5, &modules, &size));
    assert(size == sizeof(*modules) + sizeof(gsl_module_id_info_entry));
    assert(modules->num_modules == 1 &&
           modules->module_entry[0].module_id == 0x7001 &&
           modules->module_entry[0].module_iid == 0x4001);
    free(modules);

    uint8_t data[8] = {};
    size_t bytes = 0;

    assert(!gsl_get_tagged_data(&keys, 5, &tkv, nullptr, &bytes) && bytes == 4);
    bytes = 2;
    assert(gsl_get_tagged_data(&keys, 5, &tkv, data, &bytes) == AR_ENEEDMORE);
    assert(bytes == 4);
    bytes = sizeof(data);
    assert(!gsl_get_tagged_data(&keys, 5, &tkv, data, &bytes) && bytes == 4);
    assert(!std::memcmp(data, "data", 4));

    std::vector<uint8_t> tags(sizeof(gsl_tag_module_info) + 24);

    bytes = 0;
    assert(!gsl_get_tags_with_module_info(&keys, nullptr, &bytes));
    assert(bytes == tags.size());
    assert(!gsl_get_tags_with_module_info(&keys, tags.data(), &bytes));
    assert(reinterpret_cast<gsl_tag_module_info *>(tags.data())->num_tags == 3);

    acdb_query = nullptr;
}

static void startup()
{
    char directory[] = "/tmp/gsl-native-XXXXXX";

    assert(mkdtemp(directory));

    std::string path = std::string(directory) + "/bindings.json";
    Json::StreamWriterBuilder writer;

    std::ofstream(path) << Json::writeString(writer, fixture());

    auto controls = make_mixer();
    gsl_acdb_data_files files{};
    gsl_init_data data{};
    auto lost = [] { card_lost = true; };
    std::chrono::milliseconds wait{};

    data.acdb_files = &files;
    card_mixer = &controls;
    controls.controls.at("hardware3").enabled = 1;

    assert(gsl_native::init(data, "CARD", path + ".missing", wait, lost) ==
           -ENOENT);
    assert(gsl_native::init(data, "OTHER", path, wait, lost) == -ENODEV);
    acdb_error = AR_EFAILED;
    assert(gsl_native::init(data, "CARD", path, wait, lost) == -EIO);
    acdb_error = 0;
    assert(!open_mixers && !acdb_users && card_watch_end < 0);

    assert(!gsl_native::init(data, "CARD", path, wait, lost));
    assert(gsl_native::init(data, "CARD", path, wait, lost) == -EALREADY);
    assert(open_mixers == 1 && acdb_users == 1 && card_watch_end >= 0);
    assert(!controls.controls.at("hardware3").enabled);
    assert(!gsl_close(open_graph(1)));
    assert(!card_lost);

    close(card_watch_end);
    card_watch_end = -1;

    for (int i = 0; i < 500 && !card_lost; i++)
        usleep(10000);

    assert(card_lost);

    gsl_deinit();
    assert(!open_mixers && !acdb_users);

    gsl_key_value_pair pair{0xa, 1};
    gsl_key_vector keys{1, &pair};
    gsl_handle_t handle = nullptr;

    assert(gsl_open(&keys, nullptr, &handle) == AR_ENOTREADY);

    card_lost = false;
    absent_scans = 1;
    assert(gsl_native::init(data, "CARD", path, wait, lost) == -ENODEV);
    absent_scans = 1;
    assert(!gsl_native::init(data, "CARD", path, std::chrono::seconds(2),
                             lost));
    gsl_deinit();
    close(card_watch_end);
    card_watch_end = -1;
    assert(!card_lost && !open_mixers && !acdb_users);

    card_mixer = nullptr;
    remove(path.c_str());
    rmdir(directory);
}

int main()
{
    blocking_pcm();
    restarts();
    parameters();
    hostless();
    mapped();
    backend_media();
    passive();
    loopback();
    routes();
    events();
    compressed();
    queries();
    startup();

    return 0;
}
