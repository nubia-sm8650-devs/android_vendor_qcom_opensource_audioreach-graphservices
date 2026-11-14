/* SPDX-License-Identifier: BSD-3-Clause */
#include "fake_card.h"

#include "gsl_native.h"

#include <gsl_intf.h>

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

int main()
{
    blocking_pcm();

    return 0;
}
