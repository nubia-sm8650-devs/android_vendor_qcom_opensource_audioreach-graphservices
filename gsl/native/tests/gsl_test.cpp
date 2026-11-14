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
    parameters();
    queries();
    startup();

    return 0;
}
