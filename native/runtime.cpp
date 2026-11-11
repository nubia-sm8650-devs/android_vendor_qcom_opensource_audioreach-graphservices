/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_runtime.h"
#include "ar_topology_pcm_controls.h"
#include "media.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <tuple>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>
#include <sound/compress_offload.h>

#ifndef SNDRV_PCM_IOCTL_ANONYMOUS_DUP
#define SNDRV_PCM_ANON_DUP_BUFFER 1
#define SNDRV_PCM_IOCTL_ANONYMOUS_DUP _IOW('A', 0x05, int)
#endif

namespace ar_topology
{
namespace
{

int result(int status)
{
    return status == -1 ? -errno : status;
}

int open_compress(unsigned card, uint32_t device, bool capture)
{
    char path[32];

    snprintf(path, sizeof(path), "/dev/snd/comprC%uD%u", card, device);

    int fd = ::open(path, (capture ? O_RDONLY : O_WRONLY) | O_CLOEXEC);

    return fd < 0 ? -errno : fd;
}

int state(const Stream &stream)
{
    snd_pcm_status status{};
    int error = pcm_ioctl(stream.handle, SNDRV_PCM_IOCTL_STATUS, &status);

    return error ? result(error) : int(status.state);
}

int run_without_host(const Stream &stream)
{
    const pcm_config &config = stream.config;
    snd_pcm_sw_params software{};
    software.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
    software.period_step = 1;
    software.avail_min = config.avail_min ? config.avail_min : 1;
    software.start_threshold = config.start_threshold;
    software.stop_threshold = config.stop_threshold;
    software.silence_threshold = config.silence_threshold;
    software.silence_size = config.silence_size;

    int status = result(pcm_ioctl(stream.handle, SNDRV_PCM_IOCTL_SW_PARAMS,
                                  &software));
    if (status)
        return status;

    software.stop_threshold = software.boundary;

    return result(pcm_ioctl(stream.handle, SNDRV_PCM_IOCTL_SW_PARAMS,
                            &software));
}

}

struct Runtime::Impl {
    struct Control {
        struct mixer_ctl *handle;
        unsigned users = 0;
    };

    struct Session {
        Plan plan;
        std::vector<Stream> streams;
        std::vector<Control *> controls;
        std::map<std::string, Control *> peers;
        std::map<Keys, std::vector<Control *>> joined;
        size_t acquired = 0;
        bool initialized = false;
        bool managed_media = false;
        std::map<BackendId, Media> media;
        std::map<BackendId, std::vector<uint32_t>> channel_maps;
        std::vector<Keys> selections;
        std::map<Keys, Keys> calibration;
        std::set<uint32_t> untuned;
        bool compress = false;
        snd_compr_params params{};
        bool configured = false;
    };

    unsigned card;
    struct mixer *mixer;
    const Bindings &bindings;
    std::mutex mutex;
    SessionId next = 0;
    std::map<SessionId, Session> sessions;
    std::map<std::string, Control> controls;

    Impl(unsigned card, struct mixer *mixer, const Bindings &bindings)
        : card(card), mixer(mixer), bindings(bindings)
    {
    }

    int frontend(SessionId id, uint32_t pcm, bool &capture) const
    {
        auto found = sessions.find(id);
        if (found == sessions.end())
            return -ENOENT;

        bool matched = false;

        for (const auto &frontend : found->second.plan.frontends) {
            if (frontend.pcm_id != pcm)
                continue;

            if (matched)
                return -EOPNOTSUPP;

            capture = frontend.capture;
            matched = true;
        }

        return matched ? 0 : -ENOENT;
    }

    int release(SessionId id)
    {
        auto found = sessions.find(id);
        if (found == sessions.end())
            return -ENOENT;

        auto &session = found->second;
        int first = 0;

        for (auto &stream : session.streams) {
            if (stream.compress >= 0) {
                ::close(stream.compress);
                stream.compress = -1;
            }

            if (!stream.handle)
                continue;

            if (pcm_is_ready(stream.handle)) {
                int status = result(pcm_stop(stream.handle));
                if (status && !first)
                    first = status;
            }

            pcm_close(stream.handle);
            stream.handle = nullptr;
        }

        while (session.acquired) {
            auto &control = *session.controls[session.acquired - 1];
            if (control.users == 1) {
                int status = result(mixer_ctl_set_value(control.handle, 0, 0));
                if (status < 0 && !first)
                    first = status;
            }

            control.users--;
            session.acquired--;
        }

        int status = drop(session, {});
        if (status && !first)
            first = status;

        while (!session.joined.empty()) {
            status = leave(session, session.joined.begin()->first);
            if (status && !first)
                first = status;
        }

        sessions.erase(found);
        follow();

        return first;
    }

    int formats(Session &session, const std::vector<HardwareConfig> *hardware,
                   std::vector<MediaWrite> &writes,
                   const Session *rerouted = nullptr,
                   const std::set<BackendId> &retained = {})
    {
        session.managed_media = hardware != nullptr;
        if (hardware && session.plan.backends.size() !=
                        session.plan.hardware.size())
            return -EOPNOTSUPP;

        if (hardware) {
            for (const auto &config : *hardware) {
                auto keys = config.selection;
                std::sort(keys.begin(), keys.end());
                auto selected = session.plan.selections.find(keys);
                if (selected == session.plan.selections.end())
                    return -ENOENT;

                std::set<BackendId> matched;

                for (const auto &[name, backend] : session.plan.backends) {
                    (void)name;
                    if (!selected->second.count(backend.graph))
                        continue;

                    if (!config.backend.first.empty() &&
                        config.backend != backend.id)
                        continue;

                    matched.insert(backend.id);
                    if (matched.size() > 1)
                        return -EOPNOTSUPP;

                    auto [prior, added] = session.media.emplace(backend.id,
                                                                config.media);
                    if (!added && !(prior->second == config.media))
                        return -EBUSY;
                }

                if (matched.empty())
                    return -EINVAL;
            }

            for (const auto &[name, backend] : session.plan.backends) {
                (void)name;
                if (!session.media.count(backend.id))
                    return -EINVAL;
            }
        }

        if (hardware) {
            for (const auto &[name, backend] : session.plan.backends) {
                (void)name;
                if (!backend.controls.count("channel_map") ||
                    session.channel_maps.count(backend.id))
                    continue;

                auto &map = session.channel_maps[backend.id];
                int status = media_channel_map(mixer, backend,
                    session.media.at(backend.id), map);
                if (status)
                    return status;
            }
        }

        std::set<BackendId> occupied;

        for (const auto &[id, prior] : sessions) {
            (void)id;
            if (!session.plan.hardware.empty() &&
                !prior.plan.hardware.empty() &&
                session.managed_media != prior.managed_media)
                return -EBUSY;

            for (const auto &[port, media] : prior.media) {
                auto requested = session.media.find(port);

                if (&prior == rerouted && !retained.count(port)) {
                    if (requested != session.media.end() &&
                        requested->second == media)
                        occupied.insert(port);

                    continue;
                }

                if (requested != session.media.end() &&
                    !(requested->second == media))
                    return -EBUSY;

                auto map = session.channel_maps.find(port);
                auto previous = prior.channel_maps.find(port);
                if (map != session.channel_maps.end() &&
                    previous != prior.channel_maps.end() &&
                    map->second != previous->second)
                    return -ESTALE;

                occupied.insert(port);
            }
        }

        if (hardware) {
            for (const auto &[name, backend] : session.plan.backends) {
                (void)name;
                if (!occupied.insert(backend.id).second)
                    continue;

                int status = media_writes(mixer, backend,
                    session.media.at(backend.id), writes);
                if (status)
                    return status;
            }
        }

        return 0;
    }

    int validate_maps(const Session &session) const noexcept
    {
        try {
            std::set<BackendId> checked;

            for (const auto &[name, backend] : session.plan.backends) {
                (void)name;
                auto expected = session.channel_maps.find(backend.id);
                if (expected == session.channel_maps.end() ||
                    !checked.insert(backend.id).second)
                    continue;

                std::vector<uint32_t> current;
                int status = media_channel_map(mixer, backend,
                    session.media.at(backend.id), current);
                if (status)
                    return status == -EINVAL ? -ESTALE : status;

                if (current != expected->second)
                    return -ESTALE;
            }

            return 0;
        } catch (const std::bad_alloc &) {
            return -ENOMEM;
        }
    }

    bool exposes_untuned(const Plan &plan, const std::set<uint32_t> &tuned,
                         const Session *skip = nullptr)
    {
        std::set<std::string> paths = plan.paths;
        std::set<uint32_t> roots;

        for (const auto &frontend : plan.frontends)
            roots.insert(frontend.graph_id);

        for (const auto &[id, session] : sessions) {
            (void)id;

            if (&session == skip)
                continue;

            paths.insert(session.plan.paths.begin(), session.plan.paths.end());

            for (const auto &frontend : session.plan.frontends)
                roots.insert(frontend.graph_id);
        }

        for (const auto &[id, session] : sessions) {
            (void)id;

            if (&session == skip)
                continue;

            for (const auto &frontend : session.plan.frontends) {
                std::set<uint32_t> reached{frontend.graph_id};
                size_t previous;

                do {
                    previous = reached.size();

                    for (const auto &entry : bindings.references()) {
                        const auto &reference = entry.second;
                        if (!paths.count(reference.path))
                            continue;

                        uint32_t source = frontend.capture ?
                            reference.destination : reference.source;
                        uint32_t sink = frontend.capture ?
                            reference.source : reference.destination;

                        if (!reached.count(source))
                            continue;

                        if (!roots.count(sink) && !tuned.count(sink))
                            return true;

                        reached.insert(sink);
                    }
                } while (previous != reached.size());
            }
        }

        return false;
    }

    int calibrate(Session &session, const std::vector<Keys> &requests,
                  const std::vector<Keys> &calibration)
    {
        for (size_t i = 0; i < requests.size(); i++) {
            auto selection = requests[i];
            std::sort(selection.begin(), selection.end());

            Keys keys = calibration.empty() ? Keys{} : calibration[i];
            auto [found, added] = session.calibration.emplace(selection, keys);
            if (added)
                session.selections.push_back(selection);
            else if (found->second != keys)
                return -EINVAL;
        }

        return 0;
    }

    int control(const std::string &name, Control *&output)
    {
        auto *handle = mixer_get_ctl_by_name(mixer, name.c_str());
        if (!handle)
            return -ENOENT;

        if (mixer_ctl_get_type(handle) != MIXER_CTL_TYPE_BOOL ||
            mixer_ctl_get_num_values(handle) != 1)
            return -EINVAL;

        auto [found, inserted] = controls.emplace(name, Control{handle});
        (void)inserted;

        if (!found->second.users) {
            int value = mixer_ctl_get_value(handle, 0);
            if (value < 0)
                return result(value);

            if (value)
                return -EBUSY;
        }

        output = &found->second;

        return 0;
    }

    std::set<std::string> reached(const Session &session) const
    {
        std::set<std::string> wanted;

        if (!session.plan.hardware.empty())
            return wanted;

        for (const auto &name : session.plan.references) {
            const auto &reference = bindings.references().at(name);
            if (reference.path.empty() ||
                session.plan.paths.count(reference.path))
                continue;

            uint32_t peer = session.plan.graphs.count(reference.source) ?
                reference.destination : reference.source;

            for (const auto &[id, other] : sessions) {
                (void)id;

                if (&other != &session && other.plan.graphs.count(peer))
                    wanted.insert(reference.path);
            }
        }

        return wanted;
    }

    int drop(Session &session, const std::set<std::string> &wanted)
    {
        int first = 0;

        for (auto entry = session.peers.begin();
             entry != session.peers.end();) {
            if (wanted.count(entry->first)) {
                entry++;
                continue;
            }

            auto *control = entry->second;

            if (control->users == 1) {
                int status = result(mixer_ctl_set_value(control->handle, 0, 0));
                if (status < 0 && !first)
                    first = status;
            }

            control->users--;
            entry = session.peers.erase(entry);
        }

        return first;
    }

    int reach(Session &session)
    {
        auto wanted = reached(session);
        int first = drop(session, wanted);

        for (const auto &name : wanted) {
            if (session.peers.count(name))
                continue;

            Control *found = nullptr;

            int status = control(name, found);
            if (!status && found->users++ == 0) {
                status = result(mixer_ctl_set_value(found->handle, 0, 1));
                if (status < 0)
                    found->users--;
                else
                    status = 0;
            }

            if (status) {
                if (!first)
                    first = status;

                continue;
            }

            session.peers.emplace(name, found);
        }

        return first;
    }

    int leave(Session &session, const Keys &keys)
    {
        auto found = session.joined.find(keys);
        if (found == session.joined.end())
            return 0;

        int first = 0;

        for (auto *control : found->second) {
            if (control->users == 1) {
                int status = result(mixer_ctl_set_value(control->handle, 0, 0));
                if (status < 0 && !first)
                    first = status;
            }

            control->users--;
        }

        session.joined.erase(found);

        return first;
    }

    void follow()
    {
        for (auto &[id, session] : sessions) {
            (void)id;
            reach(session);
        }
    }

    int resolve(const Plan &plan, std::vector<Control *> &output)
    {
        for (const auto *names : {&plan.references, &plan.paths,
                                   &plan.hardware}) {
            for (const auto &name : *names) {
                Control *found = nullptr;

                int status = control(name, found);
                if (status)
                    return status;

                output.push_back(found);
            }
        }

        return 0;
    }
};

Runtime::Runtime(unsigned card, struct mixer *mixer, const Bindings &bindings)
    : impl(new Impl(card, mixer, bindings))
{
    if (!mixer)
        throw std::invalid_argument("Missing topology mixer");
}

Runtime::~Runtime()
{
    for (auto it = impl->sessions.begin(); it != impl->sessions.end();) {
        auto id = it++->first;
        impl->release(id);
    }
}

unsigned Runtime::card_id() const
{
    return impl->card;
}

int Runtime::acquire(const std::vector<Keys> &requests,
                     const std::vector<Pcm> &pcms,
                     SessionId &id, std::vector<Stream> &streams,
                     const std::vector<Keys> &calibration)
{
    return acquire_impl(requests, pcms, nullptr, id, streams, calibration);
}

int Runtime::acquire(const std::vector<Keys> &requests,
                     const std::vector<Pcm> &pcms,
                     const std::vector<HardwareConfig> &hardware,
                     SessionId &id, std::vector<Stream> &streams,
                     const std::vector<Keys> &calibration)
{
    return acquire_impl(requests, pcms, &hardware, id, streams, calibration);
}

int Runtime::acquire_impl(const std::vector<Keys> &requests,
                           const std::vector<Pcm> &pcms,
                           const std::vector<HardwareConfig> *hardware,
                           SessionId &id, std::vector<Stream> &streams,
                           const std::vector<Keys> &calibration)
{
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (id || !streams.empty() ||
        (!calibration.empty() && calibration.size() != requests.size()))
        return -EINVAL;

    try {
        Impl::Session session;
        int status = impl->bindings.select(requests, session.plan);
        if (status)
            return status;

        std::set<uint32_t> occupied, tuned;

        for (const auto &[existing_id, existing] : impl->sessions) {
            (void)existing_id;

            for (const auto &frontend : existing.plan.frontends)
                occupied.insert(frontend.pcm_id);

            for (const auto &[graph_id, graph] : existing.plan.graphs) {
                (void)graph;
                tuned.insert(graph_id);
            }
        }

        if (pcms.size() != session.plan.frontends.size())
            return -EINVAL;

        std::set<std::pair<uint32_t, bool>> requested;
        std::vector<const Pcm *> configs;

        for (const auto &frontend : session.plan.frontends) {
            if (frontend.mode != FrontendMode::compress)
                continue;

            if (session.plan.frontends.size() != 1)
                return -EOPNOTSUPP;

            session.compress = true;
        }

        for (const auto &frontend : session.plan.frontends) {
            const Pcm *config = nullptr;

            if (occupied.count(frontend.pcm_id))
                return -EBUSY;

            for (const auto &pcm : pcms) {
                if (pcm.id == frontend.pcm_id &&
                    bool(pcm.flags & PCM_IN) == frontend.capture)
                    config = &pcm;
            }

            if (!config ||
                !requested.emplace(config->id, frontend.capture).second ||
                config->compress.has_value() != session.compress)
                return -EINVAL;

            if (session.compress)
                session.params = *config->compress;

            configs.push_back(config);
            session.streams.push_back({frontend.pcm_id, nullptr});
            session.streams.back().config = config->config;
        }

        if (impl->exposes_untuned(session.plan, tuned))
            return -EOPNOTSUPP;

        std::vector<MediaWrite> media;
        status = impl->formats(session, hardware, media);
        if (status)
            return status;

        status = impl->calibrate(session, requests, calibration);
        if (status)
            return status;

        std::vector<Calibration> defaults;

        for (const auto &[graph_id, graph] : session.plan.graphs) {
            if (tuned.count(graph_id))
                continue;

            for (const auto &selection : session.selections) {
                if (session.plan.selections.at(selection).count(graph_id)) {
                    defaults.push_back({graph, {},
                                        session.calibration.at(selection)});
                    break;
                }
            }
        }

        status = impl->resolve(session.plan, session.controls);
        if (status)
            return status;

        std::vector<Stream> output = session.streams;

        if (impl->next == UINT64_MAX)
            return -EOVERFLOW;

        auto found = impl->sessions.emplace(++impl->next, std::move(session));
        auto &active = found.first->second;
        id = found.first->first;

        for (const auto &write : media) {
            status = result(mixer_ctl_set_value(write.control, 0, write.item));
            if (status < 0)
                break;
        }

        if (status >= 0) {
            for (auto *control : active.controls) {
                active.acquired++;
                if (control->users++ == 0) {
                    status = result(mixer_ctl_set_value(control->handle, 0, 1));
                    if (status < 0)
                        break;
                }
            }
        }

        if (status >= 0)
            status = impl->reach(active);

        if (status >= 0) {
            status = 0;

            for (size_t i = 0; i < active.streams.size(); i++) {
                auto &stream = active.streams[i];
                auto config = configs[i]->config;

                if (active.compress) {
                    stream.compress = open_compress(impl->card, configs[i]->id,
                        configs[i]->flags & PCM_IN);
                    if (stream.compress < 0) {
                        status = stream.compress;
                        stream.compress = -1;
                        break;
                    }

                    continue;
                }

                stream.handle = pcm_open(impl->card, configs[i]->id,
                                          configs[i]->flags, &config);

                if (!stream.handle || !pcm_is_ready(stream.handle)) {
                    status = -EIO;
                    break;
                }
            }
        }

        if (!status)
            status = tune(impl->mixer, defaults);

        if (status) {
            impl->release(id);
            id = 0;

            return status;
        }

        for (size_t i = 0; i < output.size(); i++) {
            output[i].handle = active.streams[i].handle;
            output[i].compress = active.streams[i].compress;
        }

        streams = std::move(output);
        impl->follow();

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int Runtime::reroute(SessionId id, const std::vector<Keys> &requests,
                     const std::vector<HardwareConfig> &hardware,
                     const std::vector<Keys> &calibration,
                     ConfigureParameters configure, void *context)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    auto &session = found->second;

    if ((!calibration.empty() && calibration.size() != requests.size()) ||
        (!session.managed_media && !hardware.empty()))
        return -EINVAL;

    try {
        Impl::Session next;
        int status = impl->bindings.select(requests, next.plan);
        if (status)
            return status;

        auto identity = [](const Plan &plan) {
            std::set<std::tuple<uint32_t, uint32_t, bool, FrontendMode>> result;

            for (const auto &frontend : plan.frontends)
                result.emplace(frontend.pcm_id, frontend.graph_id,
                               frontend.capture, frontend.mode);

            return result;
        };

        if (next.plan.frontends.size() != session.plan.frontends.size() ||
            identity(next.plan) != identity(session.plan))
            return -EXDEV;

        std::set<uint32_t> tuned;

        for (const auto &[other_id, other] : impl->sessions) {
            if (other_id == id)
                continue;

            for (const auto &[graph_id, graph] : other.plan.graphs) {
                (void)graph;
                tuned.insert(graph_id);
            }
        }

        if (impl->exposes_untuned(next.plan, tuned, &session))
            return -EOPNOTSUPP;

        std::set<BackendId> retained;

        for (const auto &[name, backend] : session.plan.backends) {
            (void)name;

            if (next.plan.graphs.count(backend.graph))
                retained.insert(backend.id);
        }

        std::vector<MediaWrite> media;
        status = impl->formats(next, session.managed_media ? &hardware : nullptr,
                               media, &session, retained);
        if (status)
            return status;

        status = impl->calibrate(next, requests, calibration);
        if (status)
            return status;

        std::set<uint32_t> untuned;
        std::vector<Calibration> defaults;

        for (const auto &[graph_id, graph] : next.plan.graphs) {
            if (tuned.count(graph_id) || (session.plan.graphs.count(graph_id) &&
                                          !session.untuned.count(graph_id)))
                continue;

            untuned.insert(graph_id);

            for (const auto &selection : next.selections) {
                if (next.plan.selections.at(selection).count(graph_id)) {
                    defaults.push_back({graph, {},
                                        next.calibration.at(selection)});
                    break;
                }
            }
        }

        status = impl->resolve(next.plan, next.controls);
        if (status)
            return status;

        size_t gates = next.controls.size() - next.plan.hardware.size();
        std::set<Impl::Control *> wanted(next.controls.begin(),
                                         next.controls.end());
        std::set<Impl::Control *> held;
        std::vector<Impl::Control *> kept;
        int first = 0;

        for (size_t i = session.acquired; i--;) {
            auto *control = session.controls[i];

            if (wanted.count(control)) {
                held.insert(control);
                kept.insert(kept.begin(), control);
                continue;
            }

            if (control->users == 1) {
                status = result(mixer_ctl_set_value(control->handle, 0, 0));
                if (status < 0 && !first)
                    first = status;
            }

            control->users--;
        }

        session.plan = std::move(next.plan);
        session.media = std::move(next.media);
        session.channel_maps = std::move(next.channel_maps);
        session.selections = std::move(next.selections);
        session.calibration = std::move(next.calibration);
        session.untuned = std::move(untuned);
        session.acquired = kept.size();
        session.controls = std::move(kept);

        auto enable = [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; i++) {
                auto *control = next.controls[i];

                if (held.count(control))
                    continue;

                session.controls.push_back(control);
                session.acquired++;

                if (control->users++ == 0) {
                    int status = result(mixer_ctl_set_value(control->handle, 0,
                                                            1));
                    if (status < 0) {
                        control->users--;
                        session.acquired--;
                        session.controls.pop_back();

                        return status;
                    }
                }
            }

            return 0;
        };

        status = 0;

        for (const auto &write : media) {
            status = result(mixer_ctl_set_value(write.control, 0, write.item));
            if (status < 0)
                break;
        }

        if (status >= 0)
            status = enable(0, gates);

        if (status >= 0)
            status = tune(impl->mixer, defaults);

        if (!status && configure) {
            Parameters writer(impl->mixer, session.plan);

            status = configure(context, writer, session.streams);
        }

        if (!status) {
            session.untuned.clear();
            status = enable(gates, next.controls.size());
        }

        impl->follow();

        return status ? status : first;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int Runtime::join(SessionId id, const Keys &selection, bool enable)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    try {
        auto &session = found->second;
        auto keys = selection;

        std::sort(keys.begin(), keys.end());

        if (!enable)
            return impl->leave(session, keys);

        if (session.joined.count(keys))
            return 0;

        std::set<uint32_t> graphs;
        std::set<std::string> references;

        int status = impl->bindings.members(keys, graphs, references);
        if (status)
            return status;

        std::vector<Impl::Control *> links;

        for (const auto &name : references) {
            const auto &reference = impl->bindings.references().at(name);
            if (!graphs.count(reference.source) ||
                !graphs.count(reference.destination) ||
                session.plan.graphs.count(reference.source) ==
                session.plan.graphs.count(reference.destination))
                continue;

            Impl::Control *control = nullptr;

            status = impl->control(name, control);
            if (status)
                return status;

            links.push_back(control);
        }

        if (links.empty())
            return -ENOENT;

        auto &held = session.joined[keys];

        for (auto *control : links) {
            held.push_back(control);

            if (control->users++ == 0) {
                status = result(mixer_ctl_set_value(control->handle, 0, 1));
                if (status < 0) {
                    impl->leave(session, keys);

                    return status;
                }
            }
        }

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int Runtime::prepare(SessionId id, Configure configure, void *context)
{
    return prepare_impl(id, configure, nullptr, context);
}

int Runtime::prepare_parameters(SessionId id, ConfigureParameters configure,
                                void *context)
{
    return prepare_impl(id, nullptr, configure, context);
}

int Runtime::prepare_impl(SessionId id, Configure configure,
                          ConfigureParameters parameters, void *context)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    auto &session = found->second;

    int map_status = impl->validate_maps(session);
    if (map_status)
        return map_status;

    for (const auto &stream : session.streams) {
        if (session.compress)
            break;

        int value = state(stream);
        if (value < 0)
            return value;

        if (value != SNDRV_PCM_STATE_SETUP &&
            value != SNDRV_PCM_STATE_PREPARED)
            return -EBADFD;
    }

    if (!session.initialized) {
        Parameters writer(impl->mixer, session.plan);
        int status = parameters ? parameters(context, writer, session.streams) :
            configure ? configure(context, session.streams) : 0;
        if (status)
            return status;

        session.initialized = true;
    }

    if (session.compress) {
        if (session.configured)
            return 0;

        int status = result(::ioctl(session.streams[0].compress,
                                    SNDRV_COMPRESS_SET_PARAMS, &session.params));
        if (status)
            return status;

        session.configured = true;

        return 0;
    }

    for (const auto &stream : session.streams) {
        int value = state(stream);
        if (value < 0)
            return value;

        if (value == SNDRV_PCM_STATE_PREPARED)
            continue;

        int status = ar_topology::prepare(stream.handle, impl->mixer, {});
        if (status)
            return status;
    }

    return 0;
}

int Runtime::set_parameters(SessionId id, const void *data, size_t size,
                            size_t *applied)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    Parameters writer(impl->mixer, found->second.plan);

    return writer.set(data, size, applied);
}

int Runtime::get_parameters(SessionId id, void *data, size_t size)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    Parameters reader(impl->mixer, found->second.plan);

    return reader.get(data, size);
}

int Runtime::set_tagged_parameters(SessionId id, const Keys &gkv, uint32_t tag,
                                   const Keys &tkv)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    Parameters writer(impl->mixer, found->second.plan);
    std::vector<uint8_t> data;
    int status = writer.resolve_tag(gkv, tag, tkv, data);
    if (status || data.empty())
        return status;

    return writer.set(data.data(), data.size());
}

int Runtime::set_calibration(SessionId id, const Keys &gkv, const Keys &ckv)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    auto &session = found->second;

    try {
        auto selection = gkv;
        std::sort(selection.begin(), selection.end());

        if (selection.empty() && !session.selections.empty())
            selection = session.selections.front();

        auto cached = session.calibration.find(selection);
        if (cached == session.calibration.end())
            return -EINVAL;

        if (!ckv.empty() && !cached->second.empty() &&
            ckv.size() != cached->second.size())
            return -EINVAL;

        std::vector<Calibration> calibrations;

        for (uint32_t graph_id : session.plan.selections.at(selection))
            calibrations.push_back({session.plan.graphs.at(graph_id),
                                    cached->second, ckv});

        int status = tune(impl->mixer, calibrations);
        if (!status && !ckv.empty())
            cached->second = ckv;

        return status;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int Runtime::export_buffer(SessionId id, uint32_t pcm, int &fd, uint32_t &bytes)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    bool capture;
    int status = impl->frontend(id, pcm, capture);
    if (status)
        return status;

    if (fd >= 0)
        return -EINVAL;

    for (const auto &stream : impl->sessions.at(id).streams) {
        if (stream.id != pcm)
            continue;

        if (!stream.handle)
            return -EOPNOTSUPP;

        int mode = SNDRV_PCM_ANON_DUP_BUFFER;
        status = result(pcm_ioctl(stream.handle,
                                  SNDRV_PCM_IOCTL_ANONYMOUS_DUP, &mode));
        if (status < 0)
            return status;

        fd = status;
        bytes = pcm_frames_to_bytes(stream.handle,
                                    pcm_get_buffer_size(stream.handle));

        return 0;
    }

    return -ENOENT;
}

int Runtime::register_events(SessionId id, uint32_t pcm, const void *data,
                              size_t size)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    bool capture;
    int status = impl->frontend(id, pcm, capture);
    if (status)
        return status;

    if (!data || size < sizeof(uint32_t))
        return -EINVAL;

    uint32_t instance;
    std::memcpy(&instance, data, sizeof(instance));
    if (!impl->sessions.at(id).plan.modules.count(instance))
        return -ENOENT;

    return register_pcm_events(impl->card, pcm, capture, data, size);
}

int Runtime::sync_pcm(SessionId id, uint32_t pcm, struct snd_pcm_sync_ptr &sync)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    bool capture;
    int status = impl->frontend(id, pcm, capture);
    if (status)
        return status;

    for (const auto &stream : impl->sessions.at(id).streams) {
        if (stream.id != pcm)
            continue;

        if (!stream.handle)
            return -EOPNOTSUPP;

        auto output = sync;
        status = result(pcm_ioctl(stream.handle, SNDRV_PCM_IOCTL_SYNC_PTR,
                                  &output));
        if (!status)
            sync = output;

        return status;
    }

    return -ENOENT;
}

int Runtime::pcm_poll_fd(SessionId id, uint32_t pcm, int &fd)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    bool capture;
    int status = impl->frontend(id, pcm, capture);
    if (status)
        return status;

    for (const auto &stream : impl->sessions.at(id).streams) {
        if (stream.id != pcm)
            continue;

        if (!stream.handle)
            return -EOPNOTSUPP;

        int value = pcm_get_poll_fd(stream.handle);
        if (value < 0)
            return -ENODEV;

        fd = value;

        return 0;
    }

    return -ENOENT;
}

int Runtime::register_events(SessionId id, const void *data, size_t size)
{
    if (!data || size < sizeof(uint32_t))
        return -EINVAL;

    uint32_t instance, pcm = 0;
    std::memcpy(&instance, data, sizeof(instance));
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        auto session = impl->sessions.find(id);
        if (session == impl->sessions.end())
            return -ENOENT;

        const auto &plan = session->second.plan;
        auto module = plan.modules.find(instance);
        if (module == plan.modules.end())
            return -ENOENT;

        bool matched = false;

        for (const auto &[frontend, graphs] : plan.frontend_graphs) {
            if (!graphs.count(module->second))
                continue;

            if (matched)
                return -EOPNOTSUPP;

            pcm = frontend;
            matched = true;
        }

        if (!matched)
            return -ENOENT;
    }

    return register_events(id, pcm, data, size);
}

int Runtime::read_event(SessionId id, uint32_t pcm, std::vector<uint8_t> &data)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    bool capture;
    int status = impl->frontend(id, pcm, capture);

    return status ? status : read_pcm_event(impl->card, pcm, capture, data);
}

int Runtime::subscribe_events(SessionId id, uint32_t pcm, int &fd)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    bool capture;
    int status = impl->frontend(id, pcm, capture);

    return status ? status : subscribe_pcm_events(impl->card, pcm, capture, fd);
}

int Runtime::compress_fd(SessionId id, uint32_t pcm, int &fd)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    bool capture;
    int status = impl->frontend(id, pcm, capture);
    if (status)
        return status;

    for (const auto &stream : impl->sessions.at(id).streams) {
        if (stream.id != pcm)
            continue;

        if (stream.compress < 0)
            return -EOPNOTSUPP;

        fd = stream.compress;

        return 0;
    }

    return -ENOENT;
}

int Runtime::start(SessionId id)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    int map_status = impl->validate_maps(found->second);
    if (map_status)
        return map_status;

    if (found->second.compress)
        return found->second.configured ? 0 : -EBADFD;

    for (const auto &stream : found->second.streams) {
        int value = state(stream);
        if (value < 0)
            return value;

        if (value != SNDRV_PCM_STATE_PREPARED &&
            value != SNDRV_PCM_STATE_RUNNING)
            return -EBADFD;
    }

    const auto &session = found->second;

    for (size_t i = 0; i < session.streams.size(); i++) {
        const auto &stream = session.streams[i];
        int value = state(stream);
        if (value < 0)
            return value;

        if (value == SNDRV_PCM_STATE_RUNNING)
            continue;

        if (session.plan.frontends[i].mode == FrontendMode::hostless) {
            int status = run_without_host(stream);
            if (status)
                return status;
        }

        int status = result(pcm_ioctl(stream.handle, SNDRV_PCM_IOCTL_START,
                                      nullptr));
        if (status)
            return status;
    }

    return 0;
}

int Runtime::stop(SessionId id)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    int first = 0;

    for (const auto &stream : found->second.streams) {
        int status = stream.handle ? result(pcm_stop(stream.handle)) :
            result(::ioctl(stream.compress, SNDRV_COMPRESS_STOP));
        if (status == -EPERM && !stream.handle)
            status = 0;

        if (status && !first)
            first = status;
    }

    return first;
}

int Runtime::open(const std::vector<Keys> &requests,
                  const std::vector<Pcm> &pcms,
                  SessionId &id, std::vector<Stream> &streams,
                  Configure configure, void *context)
{
    int status = acquire(requests, pcms, id, streams);
    if (status)
        return status;

    status = prepare(id, configure, context);
    if (status) {
        streams.clear();
        close(id);
        id = 0;
    }

    return status;
}

int Runtime::close(SessionId id)
{
    std::lock_guard<std::mutex> lock(impl->mutex);

    return impl->release(id);
}

}
