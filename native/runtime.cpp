/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_runtime.h"

#include <cerrno>
#include <mutex>
#include <new>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sound/asound.h>

namespace ar_topology
{
namespace
{

int result(int status)
{
    return status == -1 ? -errno : status;
}

int state(const Stream &stream)
{
    snd_pcm_status status{};
    int error = pcm_ioctl(stream.handle, SNDRV_PCM_IOCTL_STATUS, &status);

    return error ? result(error) : int(status.state);
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
        size_t acquired = 0;
        bool initialized = false;
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

    int release(SessionId id)
    {
        auto found = sessions.find(id);
        if (found == sessions.end())
            return -ENOENT;

        auto &session = found->second;
        int first = 0;

        for (auto &stream : session.streams) {
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

        sessions.erase(found);

        return first;
    }

    bool exposes_untuned(const Plan &plan, const std::set<uint32_t> &tuned)
    {
        std::set<std::string> paths = plan.paths;
        std::set<uint32_t> roots;

        for (const auto &frontend : plan.frontends)
            roots.insert(frontend.graph_id);

        for (const auto &[id, session] : sessions) {
            (void)id;
            paths.insert(session.plan.paths.begin(), session.plan.paths.end());

            for (const auto &frontend : session.plan.frontends)
                roots.insert(frontend.graph_id);
        }

        for (const auto &[id, session] : sessions) {
            (void)id;

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

int Runtime::acquire(const std::vector<Keys> &requests,
                     const std::vector<Pcm> &pcms,
                     SessionId &id, std::vector<Stream> &streams)
{
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (id || !streams.empty())
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

        std::set<uint32_t> requested;

        for (const auto &frontend : session.plan.frontends) {
            const Pcm *config = nullptr;

            if (occupied.count(frontend.pcm_id))
                return -EBUSY;

            for (const auto &pcm : pcms) {
                if (pcm.id == frontend.pcm_id)
                    config = &pcm;
            }

            if (!config || !requested.insert(config->id).second ||
                bool(config->flags & PCM_IN) != frontend.capture)
                return -EINVAL;

            session.streams.push_back({frontend.pcm_id, nullptr});
        }

        if (impl->exposes_untuned(session.plan, tuned))
            return -EOPNOTSUPP;

        std::vector<Graph> defaults;

        for (const auto &[graph_id, graph] : session.plan.graphs) {
            if (!tuned.count(graph_id))
                defaults.push_back(graph);
        }

        for (const auto *names : {&session.plan.references, &session.plan.paths,
                                   &session.plan.hardware}) {
            for (const auto &name : *names) {
                auto *handle = mixer_get_ctl_by_name(impl->mixer, name.c_str());
                if (!handle)
                    return -ENOENT;

                if (mixer_ctl_get_type(handle) != MIXER_CTL_TYPE_BOOL ||
                    mixer_ctl_get_num_values(handle) != 1)
                    return -EINVAL;

                auto [found, inserted] = impl->controls.emplace(
                    name, Impl::Control{handle});
                (void)inserted;

                if (!found->second.users) {
                    int value = mixer_ctl_get_value(handle, 0);
                    if (value < 0)
                        return result(value);

                    if (value)
                        return -EBUSY;
                }

                session.controls.push_back(&found->second);
            }
        }

        std::vector<Stream> output = session.streams;

        if (impl->next == UINT64_MAX)
            return -EOVERFLOW;

        auto found = impl->sessions.emplace(++impl->next, std::move(session));
        auto &active = found.first->second;
        id = found.first->first;

        for (auto *control : active.controls) {
            active.acquired++;

            if (control->users++ == 0) {
                status = result(mixer_ctl_set_value(control->handle, 0, 1));
                if (status < 0)
                    break;
            }
        }

        if (status >= 0) {
            status = 0;

            for (auto &stream : active.streams) {
                for (const auto &pcm : pcms) {
                    if (pcm.id != stream.id)
                        continue;

                    auto config = pcm.config;
                    stream.handle = pcm_open(impl->card, pcm.id, pcm.flags,
                                              &config);
                }

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

        for (size_t i = 0; i < output.size(); i++)
            output[i].handle = active.streams[i].handle;

        streams = std::move(output);

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int Runtime::prepare(SessionId id, Configure configure, void *context)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    auto &session = found->second;
    for (const auto &stream : session.streams) {
        int value = state(stream);
        if (value < 0)
            return value;

        if (value != SNDRV_PCM_STATE_SETUP &&
            value != SNDRV_PCM_STATE_PREPARED)
            return -EBADFD;
    }

    if (!session.initialized) {
        int status = configure ? configure(context, session.streams) : 0;
        if (status)
            return status;

        session.initialized = true;
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

int Runtime::start(SessionId id)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto found = impl->sessions.find(id);
    if (found == impl->sessions.end())
        return -ENOENT;

    for (const auto &stream : found->second.streams) {
        int value = state(stream);
        if (value < 0)
            return value;

        if (value != SNDRV_PCM_STATE_PREPARED &&
            value != SNDRV_PCM_STATE_RUNNING)
            return -EBADFD;
    }

    for (const auto &stream : found->second.streams) {
        int value = state(stream);
        if (value < 0)
            return value;

        if (value == SNDRV_PCM_STATE_RUNNING)
            continue;

        int status = result(pcm_start(stream.handle));
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
        int status = result(pcm_stop(stream.handle));
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
