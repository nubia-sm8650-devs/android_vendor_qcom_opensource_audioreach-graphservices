/* SPDX-License-Identifier: BSD-3-Clause */
#include "gsl_native.h"

#include <ar_topology_runtime.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <vector>
#include <ar_osal_error.h>
#include <gsl_intf.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <tinyalsa/asoundlib.h>

namespace
{

using ar_topology::Keys;

struct Buffer {
    uint32_t size = 0;
    uint32_t count = 0;
    uint32_t start = 0;
    uint32_t stop = 0;
};

struct Graph {
    std::mutex mutex;
    std::vector<Keys> requests;
    std::vector<Keys> calibration;
    std::optional<gsl_cmd_configure_pcm_media> media;
    Buffer read;
    Buffer write;
    bool acquired = false;
    ar_topology::SessionId id = 0;
    std::vector<ar_topology::Pcm> configs;
    std::vector<ar_topology::Stream> streams;
};

struct Library {
    std::unique_ptr<ar_topology::Bindings> bindings;
    std::unique_ptr<ar_topology::Runtime> runtime;
    std::map<gsl_handle_t, std::shared_ptr<Graph>> graphs;
};

std::mutex library_mutex;
std::unique_ptr<Library> library;

int32_t code(int status)
{
    switch (status < 0 ? -status : status) {
    case 0:
        return AR_EOK;
    case EINVAL:
        return AR_EBADPARAM;
    case EOPNOTSUPP:
        return AR_EUNSUPPORTED;
    case ENOSPC:
        return AR_ENORESOURCE;
    case EBADR:
        return AR_EHANDLE;
    case EALREADY:
        return AR_EALREADY;
    case EPERM:
        return AR_ENOTREADY;
    case EBUSY:
        return AR_EBUSY;
    case ECANCELED:
        return AR_EABORTED;
    case EAGAIN:
        return AR_ECONTINUE;
    case ENOMEM:
        return AR_ENOMEMORY;
    case ENOENT:
    case ENODEV:
        return AR_ENOTEXIST;
    case ETIMEDOUT:
        return AR_ETIMEOUT;
    case EIO:
        return AR_EIODATA;
    default:
        return AR_EFAILED;
    }
}

int result(int status)
{
    return status < 0 && errno ? -errno : status;
}

Keys keys(const gsl_key_vector *vector)
{
    Keys value;

    if (!vector)
        return value;

    for (uint32_t i = 0; i < vector->num_kvps; i++)
        value.emplace_back(vector->kvp[i].key, vector->kvp[i].value);

    std::sort(value.begin(), value.end());

    return value;
}

struct Use {
    std::shared_ptr<Graph> graph;
    ar_topology::Runtime *runtime = nullptr;
    const ar_topology::Bindings *bindings = nullptr;
};

Use find(gsl_handle_t handle)
{
    std::lock_guard<std::mutex> lock(library_mutex);

    if (!library)
        return {};

    auto found = library->graphs.find(handle);
    if (found == library->graphs.end())
        return {};

    return {found->second, library->runtime.get(), library->bindings.get()};
}

int sample_format(const gsl_cmd_configure_pcm_media &media, pcm_format &format)
{
    if (media.bit_width == 16 && media.bits_per_sample == 16)
        format = PCM_FORMAT_S16_LE;
    else if (media.bit_width == 24 && media.bits_per_sample == 32)
        format = PCM_FORMAT_S24_LE;
    else if (media.bit_width == 24 && media.bits_per_sample == 24)
        format = PCM_FORMAT_S24_3LE;
    else if (media.bit_width == 32 && media.bits_per_sample == 32)
        format = PCM_FORMAT_S32_LE;
    else
        return -EOPNOTSUPP;

    return 0;
}

int start_on_command(const std::vector<ar_topology::Stream> &streams)
{
    for (const auto &stream : streams) {
        if (!stream.handle)
            continue;

        pcm_config config = stream.config;
        int status;

        unsigned frames = pcm_get_buffer_size(stream.handle);
        if (frames == UINT_MAX)
            return -EOVERFLOW;

        config.start_threshold = frames + 1;

        status = pcm_set_sw_config(stream.handle, &config);
        if (status)
            return result(status);
    }

    return 0;
}

int acquire(const Use &use, Graph &graph)
{
    auto &runtime = *use.runtime;
    ar_topology::Plan plan;

    int status = use.bindings->select(graph.requests, plan);
    if (status)
        return status;

    std::vector<ar_topology::Pcm> pcms;

    for (const auto &frontend : plan.frontends) {
        if (frontend.mode != ar_topology::FrontendMode::pcm)
            return -EOPNOTSUPP;

        const Buffer &buffer = frontend.capture ? graph.read : graph.write;

        if (!graph.media || !buffer.size || !buffer.count)
            return -EPERM;

        pcm_config config{};

        status = sample_format(*graph.media, config.format);
        if (status)
            return status;

        uint32_t frame = graph.media->num_channels *
                         (graph.media->bits_per_sample / 8);
        if (!graph.media->sample_rate || !frame || buffer.size % frame)
            return -EINVAL;

        config.rate = graph.media->sample_rate;
        config.channels = graph.media->num_channels;
        config.period_size = buffer.size / frame;
        config.period_count = buffer.count;
        config.start_threshold = buffer.start;
        config.stop_threshold = buffer.stop;

        pcms.push_back({frontend.pcm_id,
                        unsigned(frontend.capture ? PCM_IN : PCM_OUT), config});
    }

    if (pcms.empty())
        return -EINVAL;

    status = runtime.acquire(graph.requests, pcms, graph.id, graph.streams,
                             graph.calibration);
    if (status)
        return status;

    status = start_on_command(graph.streams);
    if (status) {
        runtime.close(graph.id);
        graph.streams.clear();

        return status;
    }

    graph.configs = std::move(pcms);
    graph.acquired = true;

    return 0;
}

int buffer(Buffer &value, const void *payload, size_t size)
{
    if (!payload || size != sizeof(gsl_cmd_configure_read_write_params))
        return -EINVAL;

    auto &params =
        *static_cast<const gsl_cmd_configure_read_write_params *>(payload);

    if (params.attributes != GSL_DATA_MODE_BLOCKING)
        return -EOPNOTSUPP;

    value = {params.buff_size, params.num_buffs, params.start_threshold,
             params.stop_threshold};

    return 0;
}

int command(const Use &use, Graph &graph, gsl_cmd_id id, void *payload,
            size_t size)
{
    auto &runtime = *use.runtime;

    switch (id) {
    case GSL_CMD_CONFIGURE_PCM_MEDIA:
        if (!payload || size != sizeof(gsl_cmd_configure_pcm_media))
            return -EINVAL;

        if (graph.acquired)
            return -EBUSY;

        graph.media = *static_cast<gsl_cmd_configure_pcm_media *>(payload);

        return 0;
    case GSL_CMD_CONFIGURE_READ_PARAMS:
    case GSL_CMD_CONFIGURE_WRITE_PARAMS:
        if (graph.acquired)
            return -EBUSY;

        return buffer(id == GSL_CMD_CONFIGURE_READ_PARAMS ? graph.read :
                                                             graph.write,
                      payload, size);
    case GSL_CMD_PREPARE:
        if (!graph.acquired) {
            int status = acquire(use, graph);
            if (status)
                return status;
        }

        return runtime.prepare(graph.id);
    case GSL_CMD_START:
        return graph.acquired ? runtime.start(graph.id) : -EPERM;
    case GSL_CMD_STOP:
        if (payload)
            return -EOPNOTSUPP;

        return graph.acquired ? runtime.stop(graph.id) : 0;
    default:
        return -EOPNOTSUPP;
    }
}

int transfer(Graph &graph, bool capture, gsl_buff *data, uint32_t *moved)
{
    if (!data || !moved)
        return -EINVAL;

    if (!graph.acquired)
        return -EPERM;

    if (data->flags || data->metadata_size || data->alloc_info.alloc_size)
        return -EOPNOTSUPP;

    struct pcm *pcm = nullptr;

    for (size_t i = 0; i < graph.configs.size(); i++) {
        if (bool(graph.configs[i].flags & PCM_IN) != capture)
            continue;

        if (pcm)
            return -EOPNOTSUPP;

        pcm = graph.streams[i].handle;
    }

    if (!pcm)
        return -EINVAL;

    unsigned frame = pcm_frames_to_bytes(pcm, 1);
    if (!frame || data->size % frame || (data->size && !data->addr))
        return -EINVAL;

    *moved = 0;

    if (!data->size)
        return 0;

    snd_pcm_status state{};

    int status = pcm_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &state);
    if (status)
        return result(status);

    switch (state.state) {
    case SNDRV_PCM_STATE_RUNNING:
        break;
    case SNDRV_PCM_STATE_PREPARED:
        if (capture)
            return -EPERM;

        break;
    case SNDRV_PCM_STATE_XRUN:
        return -EPIPE;
    case SNDRV_PCM_STATE_DISCONNECTED:
        return -ENODEV;
    default:
        return -EPERM;
    }

    snd_xferi frames{};

    frames.buf = data->addr;
    frames.frames = data->size / frame;

    if (state.state == SNDRV_PCM_STATE_PREPARED) {
        frames.frames = std::min(frames.frames, state.avail);
        if (!frames.frames)
            return -EAGAIN;
    }

    status = pcm_ioctl(pcm, capture ? SNDRV_PCM_IOCTL_READI_FRAMES :
                                      SNDRV_PCM_IOCTL_WRITEI_FRAMES, &frames);
    if (status)
        return result(status);

    if (frames.result < 0)
        return frames.result;

    *moved = uint32_t(frames.result) * frame;

    return 0;
}

int32_t move(gsl_handle_t handle, bool capture, gsl_buff *data, uint32_t *moved)
{
    auto use = find(handle);

    if (!use.graph)
        return AR_EHANDLE;

    std::lock_guard<std::mutex> lock(use.graph->mutex);

    return code(transfer(*use.graph, capture, data, moved));
}

}

namespace gsl_native
{

int start(unsigned card, struct mixer *mixer,
          std::unique_ptr<ar_topology::Bindings> bindings)
{
    std::lock_guard<std::mutex> lock(library_mutex);

    if (library)
        return -EALREADY;

    if (!mixer || !bindings)
        return -EINVAL;

    try {
        std::unique_ptr<Library> pending(new Library);

        pending->bindings = std::move(bindings);
        pending->runtime.reset(new ar_topology::Runtime(card, mixer,
                                                        *pending->bindings));

        int status = pending->runtime->reset();
        if (status)
            return status;

        library = std::move(pending);

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

void stop()
{
    std::lock_guard<std::mutex> lock(library_mutex);

    if (!library)
        return;

    for (auto &[handle, graph] : library->graphs) {
        (void)handle;

        if (graph->acquired)
            library->runtime->close(graph->id);
    }

    library.reset();
}

}

extern "C" int32_t gsl_open(const struct gsl_key_vector *graph_key_vect,
                            const struct gsl_key_vector *cal_key_vect,
                            gsl_handle_t *graph_handle)
{
    if (!graph_key_vect || !graph_key_vect->num_kvps || !graph_handle)
        return AR_EBADPARAM;

    std::lock_guard<std::mutex> lock(library_mutex);

    if (!library)
        return AR_ENOTREADY;

    try {
        auto graph = std::make_shared<Graph>();

        graph->requests.push_back(keys(graph_key_vect));
        graph->calibration.push_back(keys(cal_key_vect));

        ar_topology::Plan plan;

        int status = library->bindings->select(graph->requests, plan);
        if (status)
            return code(status);

        library->graphs.emplace(graph.get(), graph);
        *graph_handle = graph.get();

        return AR_EOK;
    } catch (const std::bad_alloc &) {
        return AR_ENOMEMORY;
    }
}

extern "C" int32_t gsl_close(gsl_handle_t graph_handle)
{
    std::shared_ptr<Graph> graph;
    ar_topology::Runtime *runtime;

    {
        std::lock_guard<std::mutex> lock(library_mutex);

        if (!library)
            return AR_ENOTREADY;

        auto found = library->graphs.find(graph_handle);
        if (found == library->graphs.end())
            return AR_EHANDLE;

        graph = std::move(found->second);
        library->graphs.erase(found);
        runtime = library->runtime.get();
    }

    std::lock_guard<std::mutex> lock(graph->mutex);

    return graph->acquired ? code(runtime->close(graph->id)) : AR_EOK;
}

extern "C" int32_t gsl_ioctl(gsl_handle_t graph_handle, enum gsl_cmd_id cmd_id,
                             void *cmd_payload, size_t cmd_payload_sz)
{
    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    std::lock_guard<std::mutex> lock(use.graph->mutex);

    try {
        return code(command(use, *use.graph, cmd_id, cmd_payload,
                            cmd_payload_sz));
    } catch (const std::bad_alloc &) {
        return AR_ENOMEMORY;
    }
}

extern "C" int32_t gsl_read(gsl_handle_t graph_handle, uint32_t,
                            struct gsl_buff *buff, uint32_t *filled_size)
{
    return move(graph_handle, true, buff, filled_size);
}

extern "C" int32_t gsl_write(gsl_handle_t graph_handle, uint32_t,
                             struct gsl_buff *buff, uint32_t *consumed_size)
{
    return move(graph_handle, false, buff, consumed_size);
}
