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
    startup();

    return 0;
}
