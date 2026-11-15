/* SPDX-License-Identifier: BSD-3-Clause */
#define LOG_TAG "gsl-native"

#include "compress.h"
#include "gsl_native.h"

#include <ar_topology_log.h>
#include <ar_topology_pcm_controls.h>
#include <ar_topology_runtime.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <apm_api.h>
#include <common_enc_dec_api.h>
#include <gapless_api.h>
#include <ar_osal_error.h>
#include <gsl_intf.h>
#include <poll.h>
#include <sound/asound.h>
#include <sys/epoll.h>
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
    std::map<Keys, std::vector<ar_topology::HardwareConfig>> hardware;
    bool managed = false;
    gsl_handle_t lender = nullptr;
    Keys lent;
    Keys lent_calibration;
    Keys replaced;
    Keys replaced_calibration;
    std::vector<ar_topology::HardwareConfig> replaced_hardware;
    std::deque<Operation> pending;
    bool initialized = false;
    gsl_cb_func_ptr callback = nullptr;
    void *client = nullptr;
    std::vector<std::vector<uint8_t>> registrations;
    int notifications = -1;
    int wake = -1;
    int poller = -1;
    std::optional<snd_compr_params> codec;
    std::unique_ptr<gsl_native::CompressStream> compress;
    size_t next_pcm = 0;
    std::thread reader;
    std::optional<gsl_cmd_configure_pcm_media> media;
    Buffer read;
    Buffer write;
    bool acquired = false;
    bool started = false;
    bool flowing = false;
    int failure = 0;
    bool passive = false;
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

std::mutex command_mutex;
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

const char *reason(int32_t status)
{
#define NAME(value) case value: return #value;
    switch (status) {
    NAME(AR_EFAILED)
    NAME(AR_EBADPARAM)
    NAME(AR_EUNSUPPORTED)
    NAME(AR_ENORESOURCE)
    NAME(AR_EHANDLE)
    NAME(AR_EALREADY)
    NAME(AR_ENOTREADY)
    NAME(AR_EBUSY)
    NAME(AR_EABORTED)
    NAME(AR_ECONTINUE)
    NAME(AR_ENOMEMORY)
    NAME(AR_ENOTEXIST)
    NAME(AR_ETIMEOUT)
    NAME(AR_EIODATA)
    NAME(AR_ENEEDMORE)
    default:
        return "error";
    }
#undef NAME
}

[[gnu::format(printf, 2, 3)]]
int32_t report(int32_t status, const char *format, ...)
{
    char message[384];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (status)
        ALOGE("%s: %s", message, reason(status));
    else
        ALOGD("%s", message);

    return status;
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

const char *name(gsl_cmd_id id)
{
#define NAME(value) case GSL_CMD_##value: return #value;
    switch (id) {
    NAME(START)
    NAME(PREPARE)
    NAME(FLUSH)
    NAME(STOP)
    NAME(ADD_GRAPH)
    NAME(REMOVE_GRAPH)
    NAME(CHANGE_GRAPH)
    NAME(CONFIGURE_WRITE_PARAMS)
    NAME(CONFIGURE_READ_PARAMS)
    NAME(EOS)
    NAME(GET_WRITE_BUFF_INFO)
    NAME(GET_READ_BUFF_INFO)
    NAME(GET_WRITE_POS_BUFF_INFO)
    NAME(GET_READ_POS_BUFF_INFO)
    NAME(REGISTER_CUSTOM_EVENT)
    NAME(SUSPEND)
    NAME(CLOSE_WITH_PROPS)
    NAME(CONFIGURE_PCM_MEDIA)
    NAME(CONFIGURE_COMPRESS_MEDIA)
    NAME(CONFIGURE_BACKEND_MEDIA)
    default:
        return "command";
    }
#undef NAME
}

std::string detail(gsl_cmd_id id, const void *payload, size_t size)
{
    char value[160] = "";

    if (!payload)
        return value;

    switch (id) {
    case GSL_CMD_CONFIGURE_PCM_MEDIA:
        if (size == sizeof(gsl_cmd_configure_pcm_media)) {
            auto &media =
                *static_cast<const gsl_cmd_configure_pcm_media *>(payload);

            snprintf(value, sizeof(value), " %u Hz, %u channels, %u bits in %u",
                     media.sample_rate, media.num_channels, media.bit_width,
                     media.bits_per_sample);
        }

        break;
    case GSL_CMD_CONFIGURE_READ_PARAMS:
    case GSL_CMD_CONFIGURE_WRITE_PARAMS:
        if (size == sizeof(gsl_cmd_configure_read_write_params)) {
            auto &buffers = *static_cast<
                const gsl_cmd_configure_read_write_params *>(payload);

            snprintf(value, sizeof(value), " %u buffers of %u bytes, mode %#x",
                     buffers.num_buffs, buffers.buff_size,
                     buffers.attributes & GSL_ATTRIBUTES_DATA_MODE_MASK);
        }

        break;
    case GSL_CMD_CONFIGURE_COMPRESS_MEDIA:
        if (size == sizeof(snd_compr_params)) {
            auto &params = *static_cast<const snd_compr_params *>(payload);

            snprintf(value, sizeof(value), " codec %u, %u Hz, %u channels, "
                     "%u fragments of %u bytes", params.codec.id,
                     params.codec.sample_rate, params.codec.ch_in,
                     params.buffer.fragments, params.buffer.fragment_size);
        }

        break;
    case GSL_CMD_CONFIGURE_BACKEND_MEDIA:
        if (size == sizeof(gsl_cmd_configure_backend_media)) {
            auto &media =
                *static_cast<const gsl_cmd_configure_backend_media *>(payload);

            snprintf(value, sizeof(value), " backend %u, %u Hz, %u channels, "
                     "%u bits in %u, data format %u for ", media.backend,
                     media.sample_rate, media.num_channels, media.bit_width,
                     media.bits_per_sample, media.data_format);

            return value + ar_topology::text(keys(&media.graph_key_vector));
        }

        break;
    case GSL_CMD_ADD_GRAPH:
    case GSL_CMD_CHANGE_GRAPH:
        if (size == sizeof(gsl_cmd_graph_select)) {
            auto &select = *static_cast<const gsl_cmd_graph_select *>(payload);

            return " " + ar_topology::text(keys(&select.graph_key_vector)) +
                   " calibration " +
                   ar_topology::text(keys(&select.cal_key_vect));
        }

        break;
    case GSL_CMD_REMOVE_GRAPH:
        if (size == sizeof(gsl_cmd_remove_graph))
            return " " + ar_topology::text(keys(&static_cast<
                const gsl_cmd_remove_graph *>(payload)->graph_key_vector));

        break;
    case GSL_CMD_REGISTER_CUSTOM_EVENT:
        if (size >= sizeof(gsl_cmd_register_custom_event)) {
            auto &event =
                *static_cast<const gsl_cmd_register_custom_event *>(payload);

            snprintf(value, sizeof(value), " module %#x event %#x",
                     event.module_instance_id, event.event_id);
        }

        break;
    case GSL_CMD_STOP:
    case GSL_CMD_CLOSE_WITH_PROPS:
        return " with properties, ignored";
    default:
        break;
    }

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

/*
 * The DSP keeps a graph running when the client supplies no data. A PCM does
 * the same, and starts empty, only with its stop threshold at the boundary.
 */
int keep_running(struct pcm *pcm, const pcm_config &config, unsigned frames,
                 bool mapped)
{
    int type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC;

    int status = pcm_ioctl(pcm, SNDRV_PCM_IOCTL_TTSTAMP, &type);
    if (status)
        return result(status);

    snd_pcm_sw_params software{};

    software.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
    software.period_step = 1;
    software.avail_min = config.avail_min ? config.avail_min :
                         mapped ? config.period_size : 1;
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

        bool mapped = pcms[i].flags & PCM_MMAP;

        if (mapped || !(pcms[i].flags & PCM_IN))
            status = keep_running(stream.handle, config, frames, mapped);
        else
            status = result(pcm_set_sw_config(stream.handle, &config));

        if (status)
            return status;
    }

    return 0;
}

int watch(const Use &use, int fd);
int borrow(const Use &use);

int resolve(const ar_topology::Bindings &bindings,
            const std::vector<Keys> &requests, ar_topology::Plan &plan,
            bool &passive)
{
    int status = bindings.select(requests, plan);

    passive = false;

    if (status != -EOPNOTSUPP)
        return status;

    for (const auto &request : requests) {
        std::set<uint32_t> graphs;
        std::set<std::string> references;

        int member = bindings.members(request, graphs, references);
        if (member)
            return member;
    }

    ar_topology::Plan partial;

    for (const auto &request : requests) {
        if (!bindings.select({request}, partial))
            return status;
    }

    passive = true;

    return 0;
}

std::vector<ar_topology::HardwareConfig>
backends(const Graph &graph, const std::vector<Keys> &requests)
{
    std::vector<ar_topology::HardwareConfig> configs;

    for (const auto &request : requests) {
        auto found = graph.hardware.find(request);

        if (found != graph.hardware.end())
            configs.insert(configs.end(), found->second.begin(),
                           found->second.end());
    }

    return configs;
}

int acquire(const Use &use, Graph &graph)
{
    auto &runtime = *use.runtime;
    ar_topology::Plan plan;

    int status = borrow(use);
    if (status)
        return status;

    status = resolve(*use.bindings, graph.requests, plan, graph.passive);
    if (status || graph.passive)
        return status;

    std::vector<ar_topology::Pcm> pcms;
    std::vector<ar_topology::FrontendMode> modes;

    for (const auto &frontend : plan.frontends) {
        bool hostless = frontend.mode == ar_topology::FrontendMode::hostless;
        unsigned direction = frontend.capture ? PCM_IN : PCM_OUT;

        if (frontend.mode == ar_topology::FrontendMode::compress) {
            if (!graph.codec)
                return -EPERM;

            pcms.push_back({frontend.pcm_id, direction, {}, *graph.codec});
            modes.push_back(frontend.mode);

            continue;
        }

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

        unsigned flags = direction;

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

    auto hardware = backends(graph, graph.requests);

    status = hardware.empty() ?
        runtime.acquire(graph.requests, pcms, graph.id, graph.streams,
                        graph.calibration) :
        runtime.acquire(graph.requests, pcms, hardware, graph.id,
                        graph.streams, graph.calibration);
    if (status)
        return status;

    graph.managed = !hardware.empty();

    status = start_on_command(pcms, graph.streams);
    if (status) {
        runtime.close(graph.id);
        graph.streams.clear();

        return status;
    }

    graph.configs = std::move(pcms);
    graph.modes = std::move(modes);
    graph.acquired = true;

    ALOGD("%p runs as session %llu", use.graph.get(),
          (unsigned long long)graph.id);

    if (graph.modes[0] != ar_topology::FrontendMode::compress)
        return 0;

    int fd = -1;

    status = runtime.compress_fd(graph.id, graph.configs[0].id, fd);

    if (!status) {
        try {
            graph.compress.reset(new gsl_native::CompressStream(fd,
                *graph.codec, graph.configs[0].flags & PCM_IN));
            status = watch(use, graph.compress->event_fd());
        } catch (const std::system_error &error) {
            status = -error.code().value();
        }
    }

    if (status) {
        graph.compress.reset();
        runtime.close(graph.id);
        graph.streams.clear();
        graph.acquired = false;
    }

    return status;
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

struct Notice {
    uint32_t module;
    uint32_t event;
    std::vector<uint8_t> payload;
};

void collect(Graph &graph, ar_topology::Runtime &runtime, bool notified,
             std::vector<Notice> &notices)
{
    if (graph.compress) {
        gsl_native::CompressStream::Event event;

        while (!graph.compress->next_event(event)) {
            using Event = gsl_native::CompressStream::Event;

            if (event == Event::early_eos)
                notices.push_back({MODULE_ID_GAPLESS, EVENT_ID_EARLY_EOS, {}});
            else
                notices.push_back({GSL_EVENT_SRC_MODULE_ID_GSL,
                    uint32_t(event == Event::eos ? GSL_EVENT_ID_EOS :
                                                   GSL_EVENT_ID_WRITE_DONE),
                    {}});
        }
    }

    if (!notified ||
        ar_topology::consume_pcm_notifications(graph.notifications))
        return;

    /*
     * The kernel merges the notifications of events that it queues before
     * the client reads the first one, so read until the queues are empty.
     */
    for (;;) {
        std::vector<uint8_t> data;
        int status = -EAGAIN;

        for (size_t i = 0; i < graph.configs.size(); i++) {
            uint32_t pcm = graph.configs[graph.next_pcm].id;

            graph.next_pcm = (graph.next_pcm + 1) % graph.configs.size();

            status = runtime.read_event(graph.id, pcm, data);
            if (status != -EAGAIN)
                break;
        }

        if (status == -EOVERFLOW)
            continue;

        uint32_t header[3];

        if (status || data.size() < sizeof(header))
            return;

        std::memcpy(header, data.data(), sizeof(header));
        if (header[2] != data.size() - sizeof(header))
            return;

        notices.push_back({header[0], header[1],
                           {data.begin() + sizeof(header), data.end()}});
    }
}

void deliver(std::shared_ptr<Graph> graph, ar_topology::Runtime *runtime)
{
    for (;;) {
        epoll_event ready[3];

        int count = epoll_wait(graph->poller, ready, 3, -1);
        if (count < 0) {
            if (errno == EINTR)
                continue;

            return;
        }

        bool notified = false;

        for (int i = 0; i < count; i++) {
            if (ready[i].data.fd == graph->wake)
                return;

            notified |= ready[i].data.fd == graph->notifications;
        }

        std::vector<Notice> notices;
        gsl_cb_func_ptr callback;
        void *client;

        {
            std::lock_guard<std::mutex> lock(graph->mutex);

            collect(*graph, *runtime, notified, notices);
            callback = graph->callback;
            client = graph->client;
        }

        for (auto &notice : notices) {
            if (!callback)
                break;

            gsl_event_cb_params event{notice.module, notice.event,
                uint32_t(notice.payload.size()),
                notice.payload.empty() ? nullptr : notice.payload.data()};

            if (notice.module == GSL_EVENT_SRC_MODULE_ID_GSL &&
                notice.event != GSL_EVENT_ID_EOS)
                ALOGV("%p event %#x", graph.get(), notice.event);
            else
                ALOGD("%p event %#x from module %#x, %zu bytes", graph.get(),
                      notice.event, notice.module, notice.payload.size());

            callback(&event, client);
        }
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

    if (graph.poller >= 0)
        ::close(graph.poller);

    if (graph.data_fd >= 0)
        ::close(graph.data_fd);

    graph.notifications = graph.wake = graph.poller = graph.data_fd = -1;
}

int watch(const Use &use, int fd)
{
    auto &graph = *use.graph;
    epoll_event event{};

    event.events = EPOLLIN;

    if (graph.poller < 0) {
        graph.poller = epoll_create1(EPOLL_CLOEXEC);
        if (graph.poller < 0)
            return -errno;
    }

    if (graph.wake < 0) {
        graph.wake = eventfd(0, EFD_CLOEXEC);
        if (graph.wake < 0)
            return -errno;

        event.data.fd = graph.wake;

        if (epoll_ctl(graph.poller, EPOLL_CTL_ADD, graph.wake, &event))
            return -errno;
    }

    event.data.fd = fd;

    if (epoll_ctl(graph.poller, EPOLL_CTL_ADD, fd, &event))
        return -errno;

    if (!graph.reader.joinable())
        graph.reader = std::thread(deliver, use.graph, use.runtime);

    return 0;
}

int subscribe(const Use &use)
{
    auto &graph = *use.graph;

    if (graph.notifications >= 0)
        return 0;

    int status = use.runtime->subscribe_events(graph.id, graph.configs[0].id,
                                               graph.notifications);
    if (status)
        return status;

    return watch(use, graph.notifications);
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
        std::vector<ar_topology::HardwareConfig> hardware;

        if (graph.managed)
            hardware = backends(graph, requests);

        int status = use.runtime->reroute(graph.id, requests, hardware,
                                          calibration);
        if (status)
            return status;
    } else {
        ar_topology::Plan plan;

        int status = resolve(*use.bindings, requests, plan, graph.passive);
        if (status)
            return status;
    }

    graph.requests = std::move(requests);
    graph.calibration = std::move(calibration);

    for (auto entry = graph.hardware.begin(); entry != graph.hardware.end();) {
        bool used = std::find(graph.requests.begin(), graph.requests.end(),
                              entry->first) != graph.requests.end();

        entry = used ? std::next(entry) : graph.hardware.erase(entry);
    }

    return 0;
}

Graph *lender(const Graph &graph)
{
    if (!graph.lender)
        return nullptr;

    std::lock_guard<std::mutex> lock(library_mutex);

    auto found = library->graphs.find(graph.lender);
    if (found == library->graphs.end())
        return nullptr;

    auto &requests = found->second->requests;

    if (std::find(requests.begin(), requests.end(), graph.lent) ==
        requests.end())
        return nullptr;

    return found->second.get();
}

int exchange(ar_topology::Runtime &runtime, Graph &owner, const Keys &from,
             const Keys &to, const Keys &calibration,
             std::vector<ar_topology::HardwareConfig> configs)
{
    auto requests = owner.requests;
    auto calibrations = owner.calibration;
    auto hardware = owner.hardware;
    size_t index = std::find(requests.begin(), requests.end(), from) -
                   requests.begin();

    requests[index] = to;
    calibrations[index] = calibration;
    hardware.erase(from);

    for (auto &config : configs)
        config.selection = to;

    if (!configs.empty())
        hardware[to] = std::move(configs);

    if (owner.acquired) {
        std::vector<ar_topology::HardwareConfig> list;

        if (owner.managed) {
            for (const auto &request : requests) {
                auto found = hardware.find(request);

                if (found != hardware.end())
                    list.insert(list.end(), found->second.begin(),
                                found->second.end());
            }
        }

        int status = runtime.reroute(owner.id, requests, list, calibrations);
        if (status)
            return status;
    }

    owner.requests = std::move(requests);
    owner.calibration = std::move(calibrations);
    owner.hardware = std::move(hardware);

    return 0;
}

int forward(ar_topology::Runtime &runtime, Graph &graph, Graph &owner)
{
    int status = 0;

    if (!owner.initialized)
        return 0;

    while (!graph.pending.empty()) {
        auto &operation = graph.pending.front();
        int written = operation.resolved ?
            runtime.set_parameters(owner.id, operation.data.data(),
                                   operation.data.size()) :
            runtime.set_tagged_parameters(owner.id, graph.lent, operation.tag,
                                          operation.tkv);

        if (!status)
            status = written;

        graph.pending.pop_front();
    }

    return status;
}

int join(ar_topology::Runtime &runtime, Graph &graph, gsl_handle_t handle,
         Graph &owner, const Keys &shared)
{
    size_t index = std::find(owner.requests.begin(), owner.requests.end(),
                             shared) - owner.requests.begin();
    Keys prior = owner.calibration[index];
    std::vector<ar_topology::HardwareConfig> kept, configs;

    if (owner.hardware.count(shared))
        kept = owner.hardware.at(shared);

    configs = kept;

    for (const auto &[key, values] : graph.hardware) {
        (void)key;
        configs.insert(configs.end(), values.begin(), values.end());
    }

    int status = exchange(runtime, owner, shared, graph.lent,
                          graph.lent_calibration, std::move(configs));
    if (status)
        return status;

    graph.lender = handle;
    graph.replaced = shared;
    graph.replaced_calibration = std::move(prior);
    graph.replaced_hardware = std::move(kept);

    return forward(runtime, graph, owner);
}

bool within(const Keys &whole, const Keys &part)
{
    return whole != part &&
        std::includes(whole.begin(), whole.end(), part.begin(), part.end());
}

int lend(const Use &use, const Keys &added, const Keys &calibration)
{
    auto &graph = *use.graph;
    ar_topology::Plan plan;

    if (use.bindings->select({added}, plan))
        return -ENOENT;

    graph.lent = added;
    graph.lent_calibration = calibration;

    Graph *owner = nullptr;
    gsl_handle_t handle = nullptr;
    Keys shared;

    {
        std::lock_guard<std::mutex> lock(library_mutex);

        for (auto &[key, other] : library->graphs) {
            if (other.get() == &graph)
                continue;

            for (const auto &request : other->requests) {
                if (!within(added, request))
                    continue;

                owner = other.get();
                handle = key;
                shared = request;
            }
        }
    }

    if (!owner)
        return 0;

    int status = join(*use.runtime, graph, handle, *owner, shared);
    if (status && !graph.lender)
        graph.lent.clear();

    ALOGD("%p routes %s through %p, status %d", &graph,
          ar_topology::text(added).c_str(), handle, status);

    return status;
}

int borrow(const Use &use)
{
    auto &graph = *use.graph;
    std::vector<Graph *> waiting;

    {
        std::lock_guard<std::mutex> lock(library_mutex);

        for (auto &[key, other] : library->graphs) {
            (void)key;

            if (other.get() != &graph && !other->lent.empty() &&
                !other->lender)
                waiting.push_back(other.get());
        }
    }

    for (auto *other : waiting) {
        for (const auto &request : graph.requests) {
            if (!within(other->lent, request))
                continue;

            int status = join(*use.runtime, *other, use.graph.get(), graph,
                              Keys(request));

            ALOGD("%p routes %s through %p, status %d", other,
                  ar_topology::text(other->lent).c_str(), use.graph.get(),
                  status);

            if (status)
                return status;

            break;
        }
    }

    return 0;
}

int settle(const Use &use)
{
    std::vector<Graph *> borrowers;
    int status = 0;

    {
        std::lock_guard<std::mutex> lock(library_mutex);

        for (auto &[key, other] : library->graphs) {
            (void)key;

            if (other->lender == use.graph.get())
                borrowers.push_back(other.get());
        }
    }

    for (auto *other : borrowers) {
        int written = forward(*use.runtime, *other, *use.graph);

        if (!status)
            status = written;
    }

    return status;
}

int reclaim(ar_topology::Runtime &runtime, Graph &graph)
{
    Graph *owner = lender(graph);
    int status = 0;

    if (owner)
        status = exchange(runtime, *owner, graph.lent, graph.replaced,
                          graph.replaced_calibration, graph.replaced_hardware);

    ALOGD("%p stops routing %s, status %d", &graph,
          ar_topology::text(graph.lent).c_str(), status);

    graph.lender = nullptr;
    graph.lent.clear();
    graph.lent_calibration.clear();
    graph.replaced.clear();
    graph.replaced_calibration.clear();
    graph.replaced_hardware.clear();

    return status;
}

int backend(const Use &use, const void *payload, size_t size)
{
    if (!payload || size != sizeof(gsl_cmd_configure_backend_media))
        return -EINVAL;

    auto &request =
        *static_cast<const gsl_cmd_configure_backend_media *>(payload);
    const auto &aifs = use.bindings->aifs();
    auto found = aifs.find(request.backend);

    if (found == aifs.end())
        return -ENOENT;

    gsl_cmd_configure_pcm_media media{request.sample_rate,
        request.num_channels, request.bit_width, request.bits_per_sample};
    pcm_format format;

    int status = sample_format(media, format);
    if (status)
        return status;

    const char *name = format == PCM_FORMAT_S16_LE ? "S16_LE" :
                       format == PCM_FORMAT_S24_LE ? "S24_LE" :
                       format == PCM_FORMAT_S24_3LE ? "S24_3LE" : "S32_LE";
    Keys selection = keys(&request.graph_key_vector);

    if (selection.empty() || !request.sample_rate || !request.num_channels)
        return -EINVAL;

    ar_topology::HardwareConfig config{selection,
        {request.sample_rate, request.num_channels, request.data_format, name},
        found->second.id};
    auto &configs = use.graph->hardware[selection];

    for (auto &known : configs) {
        if (known.backend != config.backend)
            continue;

        known = std::move(config);

        return 0;
    }

    configs.push_back(std::move(config));

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

        if (!graph.lent.empty() && removed == graph.lent)
            return reclaim(*use.runtime, graph);

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

    if (id == GSL_CMD_ADD_GRAPH && graph.passive && graph.lent.empty()) {
        int status = lend(use, added, keys(&selection.cal_key_vect));
        if (status != -ENOENT)
            return status;
    }

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

bool silence(const uint8_t *payload, uint32_t size, bool &trailing,
             uint32_t &frames)
{
    apm_module_param_data_t header;

    if (size < sizeof(header) + sizeof(frames))
        return false;

    std::memcpy(&header, payload, sizeof(header));

    if (header.param_id != PARAM_ID_REMOVE_INITIAL_SILENCE &&
        header.param_id != PARAM_ID_REMOVE_TRAILING_SILENCE)
        return false;

    trailing = header.param_id == PARAM_ID_REMOVE_TRAILING_SILENCE;
    std::memcpy(&frames, payload + sizeof(header), sizeof(frames));

    return true;
}

int buffer(Buffer &value, const void *payload, size_t size)
{
    if (!payload || size != sizeof(gsl_cmd_configure_read_write_params))
        return -EINVAL;

    auto &params =
        *static_cast<const gsl_cmd_configure_read_write_params *>(payload);

    uint32_t mode = params.attributes & GSL_ATTRIBUTES_DATA_MODE_MASK;

    if (mode != GSL_DATA_MODE_BLOCKING && mode != GSL_DATA_MODE_PUSH_PULL &&
        mode != GSL_DATA_MODE_NON_BLOCKING)
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

        if (graph.passive)
            return 0;

        int status = runtime.prepare_parameters(graph.id, initialize, &graph);

        while (!status && !graph.registrations.empty()) {
            status = enroll(use, graph.registrations.front());
            graph.registrations.erase(graph.registrations.begin());
        }

        return status ? status : settle(use);
    }
    case GSL_CMD_REGISTER_CUSTOM_EVENT:
        return registration(use, payload, size);
    case GSL_CMD_GET_READ_BUFF_INFO:
    case GSL_CMD_GET_WRITE_BUFF_INFO:
    case GSL_CMD_GET_READ_POS_BUFF_INFO:
    case GSL_CMD_GET_WRITE_POS_BUFF_INFO:
        return describe(use, id, payload, size);
    case GSL_CMD_CONFIGURE_COMPRESS_MEDIA: {
        if (!payload || size != sizeof(snd_compr_params))
            return -EINVAL;

        auto &codec = *static_cast<const snd_compr_params *>(payload);

        if (graph.compress)
            return graph.compress->configure(codec);

        if (graph.acquired)
            return -EBUSY;

        graph.codec = codec;

        return 0;
    }
    case GSL_CMD_CONFIGURE_BACKEND_MEDIA:
        return backend(use, payload, size);
    case GSL_CMD_START: {
        if (!graph.acquired)
            return graph.passive ? 0 : -EPERM;

        int status = runtime.start(graph.id);

        if (!status && graph.compress)
            status = graph.compress->start();

        graph.started = !status;
        graph.flowing = false;

        return status;
    }
    case GSL_CMD_STOP: {
        if (payload || !graph.acquired)
            return 0;

        int first = graph.compress ? graph.compress->stop() : 0;
        int status = runtime.stop(graph.id);

        graph.started = false;

        return first ? first : status;
    }
    case GSL_CMD_EOS:
        return graph.compress ? graph.compress->eos() : -EOPNOTSUPP;
    case GSL_CMD_FLUSH: {
        if (graph.compress)
            return graph.compress->flush();

        if (!graph.acquired)
            return graph.passive ? 0 : -EPERM;

        int status = runtime.stop(graph.id);

        if (!status)
            status = runtime.prepare(graph.id);

        if (!status && graph.started)
            status = runtime.start(graph.id);

        return status;
    }
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

    if (graph.compress) {
        if (capture != graph.compress->capturing())
            return -EOPNOTSUPP;

        if (data->size && !data->addr)
            return -EINVAL;

        size_t count = 0;

        int status = capture ?
            graph.compress->read(data->addr, data->size, count) :
            graph.compress->write(data->addr, data->size, count);

        *moved = uint32_t(count);

        return status;
    }

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
        if (!capture)
            return -EPIPE;

        status = result(pcm_prepare(pcm));

        if (!status)
            status = result(pcm_ioctl(pcm, SNDRV_PCM_IOCTL_START, nullptr));

        ALOGW("%p capture PCM overran, restart status %d", &graph, status);

        if (status)
            return status;

        break;
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

    auto &graph = *use.graph;
    std::lock_guard<std::mutex> lock(graph.mutex);

    int status = transfer(graph, capture, data, moved);

    if (status && status != -EAGAIN && status != graph.failure)
        ALOGE("%p %s of %u bytes: %s", handle, capture ? "read" : "write",
              data ? data->size : 0, strerror(-status));
    else if (!status && (!graph.flowing || graph.failure))
        ALOGD("%p %s %u bytes, data flows", handle,
              capture ? "read" : "wrote", *moved);
    else if (!status)
        ALOGV("%p %s %u bytes", handle, capture ? "read" : "wrote", *moved);

    if (status != -EAGAIN) {
        graph.failure = status;
        graph.flowing = !status;
    }

    return code(status);
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
            return LOG_ERRNO(status, "card %u: route controls do not reset: %s",
                             card, strerror(-status));

        library = std::move(pending);

        ALOGI("native GSL runs on card %u", card);

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
        graph->compress.reset();

        if (graph->acquired)
            library->runtime->close(graph->id);
    }

    library.reset();
}

}

static int32_t open_graph(const struct gsl_key_vector *graph_key_vect,
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

        int status = resolve(*library->bindings, graph->requests, plan,
                             graph->passive);
        if (status)
            return code(LOG_ERRNO(status, "the bindings have no selection %s",
                ar_topology::text(graph->requests).c_str()));

        if (graph->passive)
            ALOGD("%s has no PCM and waits for a graph to route it",
                  ar_topology::text(graph->requests).c_str());

        library->graphs.emplace(graph.get(), graph);
        *graph_handle = graph.get();

        return AR_EOK;
    } catch (const std::bad_alloc &) {
        return AR_ENOMEMORY;
    }
}

static int32_t close_graph(gsl_handle_t graph_handle)
{
    std::lock_guard<std::mutex> commands(command_mutex);
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

        for (auto &[key, other] : library->graphs) {
            (void)key;

            if (other->lender == graph_handle)
                other->lender = nullptr;
        }
    }

    silence(*graph);

    std::lock_guard<std::mutex> lock(graph->mutex);

    graph->compress.reset();

    if (!graph->lent.empty())
        return code(reclaim(*runtime, *graph));

    return graph->acquired ? code(runtime->close(graph->id)) : AR_EOK;
}

extern "C" int32_t gsl_get_backends(struct gsl_backend *backends,
                                    uint32_t *num_backends)
{
    if (!num_backends)
        return AR_EBADPARAM;

    std::lock_guard<std::mutex> lock(library_mutex);

    if (!library)
        return AR_ENOTREADY;

    const auto &aifs = library->bindings->aifs();
    uint32_t capacity = *num_backends;

    *num_backends = uint32_t(aifs.size());

    if (!backends)
        return AR_EOK;

    if (capacity < aifs.size())
        return AR_ENEEDMORE;

    for (const auto &[id, aif] : aifs) {
        gsl_backend entry{};

        if (aif.aif_name.size() >= sizeof(entry.name))
            return AR_EFAILED;

        entry.id = id;
        entry.is_capture = aif.capture;
        std::memcpy(entry.name, aif.aif_name.data(), aif.aif_name.size());
        *backends++ = entry;
    }

    return AR_EOK;
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

static int32_t run_command(gsl_handle_t graph_handle, enum gsl_cmd_id cmd_id,
                           void *cmd_payload, size_t cmd_payload_sz)
{
    std::lock_guard<std::mutex> commands(command_mutex);
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

static int32_t set_parameters(gsl_handle_t graph_handle,
                              const uint8_t *payload,
                              const uint32_t payload_size)
{
    if (!payload || !payload_size)
        return AR_EBADPARAM;

    if (payload_size > uint32_t(std::numeric_limits<int32_t>::max()) - 8)
        return AR_EBADPARAM;

    std::lock_guard<std::mutex> commands(command_mutex);
    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    auto &graph = *use.graph;
    std::lock_guard<std::mutex> lock(graph.mutex);

    bool trailing;
    uint32_t frames;

    if (graph.compress && silence(payload, payload_size, trailing, frames))
        return code(graph.compress->silence(trailing, frames));

    Graph *owner = lender(graph);

    if (owner && owner->initialized)
        return code(use.runtime->set_parameters(owner->id, payload,
                                                payload_size));

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

static int32_t get_parameters(gsl_handle_t graph_handle,
                              uint8_t *payload, uint32_t size)
{
    if (!payload || !size)
        return AR_EBADPARAM;

    std::lock_guard<std::mutex> commands(command_mutex);
    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    auto &graph = *use.graph;
    std::lock_guard<std::mutex> lock(graph.mutex);

    Graph *owner = lender(graph);

    if (owner && owner->initialized)
        return code(use.runtime->get_parameters(owner->id, payload, size));

    if (!graph.initialized)
        return AR_ENOTREADY;

    return code(use.runtime->get_parameters(graph.id, payload, size));
}

static int32_t set_tagged(gsl_handle_t graph_handle,
                          const struct gsl_key_vector *graph_key_vect,
                          uint32_t tag,
                          const struct gsl_key_vector *tag_key_vect)
{
    std::lock_guard<std::mutex> commands(command_mutex);
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

        Graph *owner = lender(graph);

        if (owner && owner->initialized)
            return code(use.runtime->set_tagged_parameters(owner->id,
                graph.lent, operation.tag, operation.tkv));

        if (graph.initialized)
            return code(use.runtime->set_tagged_parameters(graph.id,
                operation.gkv, operation.tag, operation.tkv));

        graph.pending.push_back(std::move(operation));

        return AR_EOK;
    } catch (const std::bad_alloc &) {
        return AR_ENOMEMORY;
    }
}

static int32_t set_calibration(gsl_handle_t graph_handle,
                               const struct gsl_key_vector *graph_key_vect,
                               const struct gsl_key_vector *cal_key_vect)
{
    std::lock_guard<std::mutex> commands(command_mutex);
    auto use = find(graph_handle);

    if (!use.graph)
        return AR_EHANDLE;

    auto &graph = *use.graph;
    std::lock_guard<std::mutex> lock(graph.mutex);

    try {
        Keys selection = keys(graph_key_vect);
        Keys calibration = keys(cal_key_vect);

        Graph *owner = lender(graph);

        if (owner && owner->acquired)
            return code(use.runtime->set_calibration(owner->id, graph.lent,
                                                     calibration));

        if (!graph.lent.empty()) {
            graph.lent_calibration = std::move(calibration);

            return AR_EOK;
        }

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

extern "C" int32_t gsl_open(const struct gsl_key_vector *graph_key_vect,
                            const struct gsl_key_vector *cal_key_vect,
                            gsl_handle_t *graph_handle)
{
    int32_t status = open_graph(graph_key_vect, cal_key_vect, graph_handle);

    return report(status, "open %s calibration %s as %p",
                  ar_topology::text(keys(graph_key_vect)).c_str(),
                  ar_topology::text(keys(cal_key_vect)).c_str(),
                  status ? nullptr : *graph_handle);
}

extern "C" int32_t gsl_close(gsl_handle_t graph_handle)
{
    return report(close_graph(graph_handle), "%p close", graph_handle);
}

extern "C" int32_t gsl_ioctl(gsl_handle_t graph_handle, enum gsl_cmd_id cmd_id,
                             void *cmd_payload, size_t cmd_payload_sz)
{
    std::string values = detail(cmd_id, cmd_payload, cmd_payload_sz);

    return report(run_command(graph_handle, cmd_id, cmd_payload,
                              cmd_payload_sz),
                  "%p %s%s", graph_handle, name(cmd_id), values.c_str());
}

extern "C" int32_t gsl_set_custom_config(gsl_handle_t graph_handle,
                                         const uint8_t *payload,
                                         const uint32_t payload_size)
{
    uint32_t first[2] = {};

    if (payload && payload_size >= sizeof(first))
        std::memcpy(first, payload, sizeof(first));

    return report(set_parameters(graph_handle, payload, payload_size),
                  "%p set %u bytes of parameters from module %#x parameter %#x",
                  graph_handle, payload_size, first[0], first[1]);
}

extern "C" int32_t gsl_get_custom_config(gsl_handle_t graph_handle,
                                         uint8_t *payload, uint32_t size)
{
    uint32_t first[2] = {};

    if (payload && size >= sizeof(first))
        std::memcpy(first, payload, sizeof(first));

    return report(get_parameters(graph_handle, payload, size),
                  "%p get module %#x parameter %#x", graph_handle, first[0],
                  first[1]);
}

extern "C" int32_t gsl_set_config(gsl_handle_t graph_handle,
                                  const struct gsl_key_vector *graph_key_vect,
                                  uint32_t tag,
                                  const struct gsl_key_vector *tag_key_vect)
{
    return report(set_tagged(graph_handle, graph_key_vect, tag, tag_key_vect),
                  "%p set tag %#x %s on %s", graph_handle, tag,
                  ar_topology::text(keys(tag_key_vect)).c_str(),
                  ar_topology::text(keys(graph_key_vect)).c_str());
}

extern "C" int32_t gsl_set_cal(gsl_handle_t graph_handle,
                               const struct gsl_key_vector *graph_key_vect,
                               const struct gsl_key_vector *cal_key_vect)
{
    return report(set_calibration(graph_handle, graph_key_vect, cal_key_vect),
                  "%p set calibration %s on %s", graph_handle,
                  ar_topology::text(keys(cal_key_vect)).c_str(),
                  ar_topology::text(keys(graph_key_vect)).c_str());
}
