/* SPDX-License-Identifier: BSD-3-Clause */
#include "gsl_native.h"

#include <ar_topology_pcm_controls.h>
#include <ar_topology_runtime.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <vector>
#include <apm_api.h>
#include <ar_osal_error.h>
#include <gsl_intf.h>
#include <poll.h>
#include <sound/asound.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <tinyalsa/asoundlib.h>
#include <unistd.h>

namespace
{

using ar_topology::Keys;

struct Buffer {
    uint32_t size = 0;
    uint32_t count = 0;
    uint32_t start = 0;
    uint32_t stop = 0;
    uint32_t mode = GSL_DATA_MODE_BLOCKING;
};

struct Operation {
    std::vector<uint8_t> data;
    Keys gkv;
    Keys tkv;
    uint32_t tag = 0;
    bool resolved = false;
};

struct Graph {
    std::mutex mutex;
    std::vector<Keys> requests;
    std::vector<Keys> calibration;
    std::deque<Operation> pending;
    bool initialized = false;
    gsl_cb_func_ptr callback = nullptr;
    void *client = nullptr;
    std::vector<std::vector<uint8_t>> registrations;
    int notifications = -1;
    int wake = -1;
    size_t next_pcm = 0;
    std::thread reader;
    std::optional<gsl_cmd_configure_pcm_media> media;
    Buffer read;
    Buffer write;
    bool acquired = false;
    ar_topology::SessionId id = 0;
    std::vector<ar_topology::Pcm> configs;
    std::vector<ar_topology::Stream> streams;
    std::vector<ar_topology::FrontendMode> modes;
    int32_t data_fd = -1;
    uint32_t data_bytes = 0;
    int32_t position_fd = -1;
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

int map_on_command(struct pcm *pcm, const pcm_config &config, unsigned frames)
{
    int type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC;

    int status = pcm_ioctl(pcm, SNDRV_PCM_IOCTL_TTSTAMP, &type);
    if (status)
        return result(status);

    snd_pcm_sw_params software{};

    software.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
    software.period_step = 1;
    software.avail_min = config.avail_min ? config.avail_min :
                                            config.period_size;
    software.start_threshold = config.start_threshold;
    software.stop_threshold = config.stop_threshold ? config.stop_threshold :
                                                      frames;

    status = pcm_ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &software);
    if (status)
        return result(status);

    software.stop_threshold = software.boundary;

    return result(pcm_ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &software));
}

int start_on_command(const std::vector<ar_topology::Pcm> &pcms,
                     const std::vector<ar_topology::Stream> &streams)
{
    for (size_t i = 0; i < streams.size(); i++) {
        const auto &stream = streams[i];

        if (!stream.handle)
            continue;

        pcm_config config = stream.config;
        int status;

        unsigned frames = pcm_get_buffer_size(stream.handle);
        if (frames == UINT_MAX)
            return -EOVERFLOW;

        config.start_threshold = frames + 1;

        if (pcms[i].flags & PCM_MMAP)
            status = map_on_command(stream.handle, config, frames);
        else
            status = result(pcm_set_sw_config(stream.handle, &config));

        if (status)
            return status;
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
    std::vector<ar_topology::FrontendMode> modes;

    for (const auto &frontend : plan.frontends) {
        bool hostless = frontend.mode == ar_topology::FrontendMode::hostless;

        if (frontend.mode != ar_topology::FrontendMode::pcm && !hostless)
            return -EOPNOTSUPP;

        const Buffer &buffer = frontend.capture ? graph.read : graph.write;

        if (!graph.media || (!hostless && (!buffer.size || !buffer.count)))
            return -EPERM;

        pcm_config config{};

        status = sample_format(*graph.media, config.format);
        if (status)
            return status;

        uint32_t frame = graph.media->num_channels *
                         (graph.media->bits_per_sample / 8);
        if (!graph.media->sample_rate || !frame)
            return -EINVAL;

        unsigned flags = frontend.capture ? PCM_IN : PCM_OUT;

        config.rate = graph.media->sample_rate;
        config.channels = graph.media->num_channels;

        if (hostless) {
            config.period_size = config.rate / 100;
            config.period_count = 2;
        } else {
            if (buffer.size % frame)
                return -EINVAL;

            config.period_size = buffer.size / frame;
            config.period_count = buffer.count;
            config.start_threshold = buffer.start;
            config.stop_threshold = buffer.stop;

            if (buffer.mode == GSL_DATA_MODE_PUSH_PULL)
                flags |= PCM_MMAP | PCM_NOIRQ | PCM_MONOTONIC;
        }

        pcms.push_back({frontend.pcm_id, flags, config});
        modes.push_back(frontend.mode);
    }

    if (pcms.empty())
        return -EINVAL;

    status = runtime.acquire(graph.requests, pcms, graph.id, graph.streams,
                             graph.calibration);
    if (status)
        return status;

    status = start_on_command(pcms, graph.streams);
    if (status) {
        runtime.close(graph.id);
        graph.streams.clear();

        return status;
    }

    graph.configs = std::move(pcms);
    graph.modes = std::move(modes);
    graph.acquired = true;

    return 0;
}

int initialize(void *context, ar_topology::Parameters &writer,
               const std::vector<ar_topology::Stream> &) noexcept
{
    auto &graph = *static_cast<Graph *>(context);

    while (!graph.pending.empty()) {
        auto &operation = graph.pending.front();

        if (!operation.resolved) {
            int status = writer.resolve_tag(operation.gkv, operation.tag,
                                            operation.tkv, operation.data);
            if (status)
                return status;

            operation.resolved = true;
        }

        if (!operation.data.empty()) {
            int status = writer.set(operation.data.data(),
                                    operation.data.size());
            if (status)
                return status;
        }

        graph.pending.pop_front();
    }

    graph.initialized = true;

    return 0;
}

void deliver(std::shared_ptr<Graph> graph, ar_topology::Runtime *runtime)
{
    pollfd fds[2] = {{graph->notifications, POLLIN, 0},
                     {graph->wake, POLLIN, 0}};

    for (;;) {
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;

            return;
        }

        if (fds[1].revents)
            return;

        std::vector<uint8_t> data;
        gsl_cb_func_ptr callback;
        void *client;

        {
            std::lock_guard<std::mutex> lock(graph->mutex);

            if (ar_topology::consume_pcm_notifications(graph->notifications))
                return;

            int status = -EAGAIN;

            for (size_t i = 0; i < graph->configs.size(); i++) {
                uint32_t pcm = graph->configs[graph->next_pcm].id;

                graph->next_pcm = (graph->next_pcm + 1) % graph->configs.size();

                status = runtime->read_event(graph->id, pcm, data);
                if (status != -EAGAIN)
                    break;
            }

            if (status)
                continue;

            callback = graph->callback;
            client = graph->client;
        }

        uint32_t header[3];

        if (!callback || data.size() < sizeof(header))
            continue;

        std::memcpy(header, data.data(), sizeof(header));
        if (header[2] != data.size() - sizeof(header))
            continue;

        gsl_event_cb_params event{header[0], header[1], header[2],
                                  data.data() + sizeof(header)};

        callback(&event, client);
    }
}

void silence(Graph &graph)
{
    if (graph.reader.joinable()) {
        uint64_t count = 1;
        ssize_t status;

        do {
            status = ::write(graph.wake, &count, sizeof(count));
        } while (status < 0 && errno == EINTR);

        graph.reader.join();
    }

    if (graph.notifications >= 0)
        ::close(graph.notifications);

    if (graph.wake >= 0)
        ::close(graph.wake);

    if (graph.data_fd >= 0)
        ::close(graph.data_fd);

    graph.notifications = graph.wake = graph.data_fd = -1;
}

int subscribe(const Use &use)
{
    auto &graph = *use.graph;

    if (graph.reader.joinable())
        return 0;

    if (graph.notifications < 0) {
        int status = use.runtime->subscribe_events(graph.id,
            graph.configs[0].id, graph.notifications);
        if (status)
            return status;
    }

    if (graph.wake < 0) {
        graph.wake = eventfd(0, EFD_CLOEXEC);
        if (graph.wake < 0)
            return -errno;
    }

    graph.reader = std::thread(deliver, use.graph, use.runtime);

    return 0;
}

int enroll(const Use &use, const std::vector<uint8_t> &registration)
{
    int status = subscribe(use);
    if (status)
        return status;

    return use.runtime->register_events(use.graph->id, registration.data(),
                                        registration.size());
}

int registration(const Use &use, const void *payload, size_t size)
{
    auto &graph = *use.graph;

    if (!payload || size < sizeof(gsl_cmd_register_custom_event))
        return -EINVAL;

    auto &request =
        *static_cast<const gsl_cmd_register_custom_event *>(payload);

    if (request.is_register > 1 ||
        size - sizeof(request) != request.event_config_payload_size)
        return -EINVAL;

    apm_module_register_events_t header{};

    header.module_instance_id = request.module_instance_id;
    header.event_id = request.event_id;
    header.is_register = request.is_register;
    header.event_config_payload_size = request.event_config_payload_size;

    std::vector<uint8_t> data(sizeof(header) +
                              request.event_config_payload_size);

    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + sizeof(header), request.event_config_payload,
                request.event_config_payload_size);

    if (graph.acquired)
        return enroll(use, data);

    graph.registrations.push_back(std::move(data));

    return 0;
}

int route(const Use &use, std::vector<Keys> requests,
          std::vector<Keys> calibration)
{
    auto &graph = *use.graph;

    if (requests.empty())
        return -EINVAL;

    if (graph.acquired) {
        int status = use.runtime->reroute(graph.id, requests, {}, calibration);
        if (status)
            return status;
    } else {
        ar_topology::Plan plan;

        int status = use.bindings->select(requests, plan);
        if (status)
            return status;
    }

    graph.requests = std::move(requests);
    graph.calibration = std::move(calibration);

    return 0;
}

int select(const Use &use, gsl_cmd_id id, const void *payload, size_t size)
{
    auto &graph = *use.graph;
    auto requests = graph.requests;
    auto calibration = graph.calibration;

    if (id == GSL_CMD_REMOVE_GRAPH) {
        if (!payload || size != sizeof(gsl_cmd_remove_graph))
            return -EINVAL;

        Keys removed = keys(
            &static_cast<const gsl_cmd_remove_graph *>(payload)->graph_key_vector);
        auto found = std::find(requests.begin(), requests.end(), removed);

        if (found == requests.end())
            return -ENOENT;

        calibration.erase(calibration.begin() + (found - requests.begin()));
        requests.erase(found);

        return route(use, std::move(requests), std::move(calibration));
    }

    if (!payload || size != sizeof(gsl_cmd_graph_select))
        return -EINVAL;

    auto &selection = *static_cast<const gsl_cmd_graph_select *>(payload);
    Keys added = keys(&selection.graph_key_vector);

    if (added.empty())
        return -EINVAL;

    if (id == GSL_CMD_CHANGE_GRAPH) {
        requests.clear();
        calibration.clear();
    } else if (std::find(requests.begin(), requests.end(), added) !=
               requests.end()) {
        return -EALREADY;
    }

    requests.push_back(std::move(added));
    calibration.push_back(keys(&selection.cal_key_vect));

    return route(use, std::move(requests), std::move(calibration));
}

int buffer(Buffer &value, const void *payload, size_t size)
{
    if (!payload || size != sizeof(gsl_cmd_configure_read_write_params))
        return -EINVAL;

    auto &params =
        *static_cast<const gsl_cmd_configure_read_write_params *>(payload);

    uint32_t mode = params.attributes & GSL_ATTRIBUTES_DATA_MODE_MASK;

    if (mode != GSL_DATA_MODE_BLOCKING && mode != GSL_DATA_MODE_PUSH_PULL)
        return -EOPNOTSUPP;

    value = {params.buff_size, params.num_buffs, params.start_threshold,
             params.stop_threshold, mode};

    return 0;
}

int describe(const Use &use, gsl_cmd_id id, void *payload, size_t size)
{
    auto &graph = *use.graph;

    if (!payload || size < sizeof(gsl_cmd_get_shmem_buf_info))
        return -EINVAL;

    auto &info = *static_cast<gsl_cmd_get_shmem_buf_info *>(payload);

    if (!info.num_buffs || !info.buffs)
        return -EINVAL;

    if (!graph.acquired) {
        int status = acquire(use, graph);
        if (status)
            return status;
    }

    bool capture = id == GSL_CMD_GET_READ_BUFF_INFO ||
                   id == GSL_CMD_GET_READ_POS_BUFF_INFO;
    const ar_topology::Pcm *config = nullptr;

    for (const auto &candidate : graph.configs) {
        if (bool(candidate.flags & PCM_IN) != capture ||
            !(candidate.flags & PCM_MMAP))
            continue;

        if (config)
            return -EOPNOTSUPP;

        config = &candidate;
    }

    if (!config)
        return -EOPNOTSUPP;

    info.buffs[0].addr = nullptr;

    if (id == GSL_CMD_GET_READ_POS_BUFF_INFO ||
        id == GSL_CMD_GET_WRITE_POS_BUFF_INFO) {
        int fd;

        int status = use.runtime->pcm_poll_fd(graph.id, config->id, fd);
        if (status)
            return status;

        graph.position_fd = fd;
        info.size = 0;
        info.buffs[0].metadata = reinterpret_cast<uintptr_t>(&graph.position_fd);

        return 0;
    }

    if (graph.data_fd < 0) {
        int fd = -1;

        int status = use.runtime->export_buffer(graph.id, config->id, fd,
                                                graph.data_bytes);
        if (status)
            return status;

        graph.data_fd = fd;
    }

    info.size = graph.data_bytes;
    info.buffs[0].metadata = reinterpret_cast<uintptr_t>(&graph.data_fd);

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
    case GSL_CMD_PREPARE: {
        if (!graph.acquired) {
            int status = acquire(use, graph);
            if (status)
                return status;
        }

        int status = runtime.prepare_parameters(graph.id, initialize, &graph);

        while (!status && !graph.registrations.empty()) {
            status = enroll(use, graph.registrations.front());
            graph.registrations.erase(graph.registrations.begin());
        }

        return status;
    }
    case GSL_CMD_REGISTER_CUSTOM_EVENT:
        return registration(use, payload, size);
    case GSL_CMD_GET_READ_BUFF_INFO:
    case GSL_CMD_GET_WRITE_BUFF_INFO:
    case GSL_CMD_GET_READ_POS_BUFF_INFO:
    case GSL_CMD_GET_WRITE_POS_BUFF_INFO:
        return describe(use, id, payload, size);
    case GSL_CMD_START:
        return graph.acquired ? runtime.start(graph.id) : -EPERM;
    case GSL_CMD_STOP:
        if (payload)
            return 0;

        return graph.acquired ? runtime.stop(graph.id) : 0;
    case GSL_CMD_CLOSE_WITH_PROPS:
        return 0;
    case GSL_CMD_ADD_GRAPH:
    case GSL_CMD_CHANGE_GRAPH:
    case GSL_CMD_REMOVE_GRAPH:
        return select(use, id, payload, size);
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

        if (pcm || (graph.configs[i].flags & PCM_MMAP) ||
            graph.modes[i] != ar_topology::FrontendMode::pcm)
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

        silence(*graph);

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

    silence(*graph);

    std::lock_guard<std::mutex> lock(graph->mutex);

    return graph->acquired ? code(runtime->close(graph->id)) : AR_EOK;
}

extern "C" int32_t gsl_register_event_cb(gsl_handle_t graph_handle,
                                         gsl_cb_func_ptr cb, void *client_data)
{
    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    std::lock_guard<std::mutex> lock(use.graph->mutex);

    use.graph->callback = cb;
    use.graph->client = client_data;

    return AR_EOK;
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

extern "C" int32_t gsl_set_custom_config(gsl_handle_t graph_handle,
                                         const uint8_t *payload,
                                         const uint32_t payload_size)
{
    if (!payload || !payload_size)
        return AR_EBADPARAM;

    if (payload_size > uint32_t(std::numeric_limits<int32_t>::max()) - 8)
        return AR_EBADPARAM;

    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    auto &graph = *use.graph;
    std::lock_guard<std::mutex> lock(graph.mutex);

    if (graph.initialized)
        return code(use.runtime->set_parameters(graph.id, payload,
                                                payload_size));

    try {
        Operation operation;

        operation.data.assign(payload, payload + payload_size);
        operation.resolved = true;
        graph.pending.push_back(std::move(operation));

        return AR_EOK;
    } catch (const std::bad_alloc &) {
        return AR_ENOMEMORY;
    }
}

extern "C" int32_t gsl_get_custom_config(gsl_handle_t graph_handle,
                                         uint8_t *payload, uint32_t size)
{
    if (!payload || !size)
        return AR_EBADPARAM;

    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    auto &graph = *use.graph;
    std::lock_guard<std::mutex> lock(graph.mutex);

    if (!graph.initialized)
        return AR_ENOTREADY;

    return code(use.runtime->get_parameters(graph.id, payload, size));
}

extern "C" int32_t gsl_set_config(gsl_handle_t graph_handle,
                                  const struct gsl_key_vector *graph_key_vect,
                                  uint32_t tag,
                                  const struct gsl_key_vector *tag_key_vect)
{
    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    auto &graph = *use.graph;
    std::lock_guard<std::mutex> lock(graph.mutex);

    try {
        Operation operation;

        operation.gkv = keys(graph_key_vect);
        operation.tkv = keys(tag_key_vect);
        operation.tag = tag;

        if (graph.initialized)
            return code(use.runtime->set_tagged_parameters(graph.id,
                operation.gkv, operation.tag, operation.tkv));

        graph.pending.push_back(std::move(operation));

        return AR_EOK;
    } catch (const std::bad_alloc &) {
        return AR_ENOMEMORY;
    }
}

extern "C" int32_t gsl_set_cal(gsl_handle_t graph_handle,
                               const struct gsl_key_vector *graph_key_vect,
                               const struct gsl_key_vector *cal_key_vect)
{
    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    auto &graph = *use.graph;
    std::lock_guard<std::mutex> lock(graph.mutex);

    try {
        Keys selection = keys(graph_key_vect);
        Keys calibration = keys(cal_key_vect);

        if (graph.acquired)
            return code(use.runtime->set_calibration(graph.id, selection,
                                                     calibration));

        for (size_t i = 0; i < graph.requests.size(); i++) {
            if (!selection.empty() && graph.requests[i] != selection)
                continue;

            graph.calibration[i] = std::move(calibration);

            return AR_EOK;
        }

        return AR_EBADPARAM;
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
