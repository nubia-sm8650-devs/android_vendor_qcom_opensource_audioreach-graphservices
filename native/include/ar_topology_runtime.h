/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_RUNTIME_H
#define AR_TOPOLOGY_RUNTIME_H

#include "ar_topology_parameters.h"

#include <tinyalsa/asoundlib.h>

namespace ar_topology
{

struct Pcm {
    uint32_t id;
    unsigned flags;
    pcm_config config;
};

struct Stream {
    uint32_t id;
    struct pcm *handle;
};

using SessionId = uint64_t;
using Configure = int (*)(void *, const std::vector<Stream> &) noexcept;
using ConfigureParameters = int (*)(void *, Parameters &,
                                    const std::vector<Stream> &) noexcept;

class Runtime {
public:
    Runtime(unsigned card, struct mixer *mixer, const Bindings &bindings);
    ~Runtime();
    int acquire(const std::vector<Keys> &requests, const std::vector<Pcm> &pcms,
                SessionId &id, std::vector<Stream> &streams);
    int prepare(SessionId id, Configure configure = nullptr,
                 void *context = nullptr);
    int prepare_parameters(SessionId id, ConfigureParameters configure,
                            void *context = nullptr);
    int set_parameters(SessionId id, const void *data, size_t size,
                        size_t *applied = nullptr);
    int set_tagged_parameters(SessionId id, const Keys &gkv, uint32_t tag,
                               const Keys &tkv);
    int start(SessionId id);
    int stop(SessionId id);
    int open(const std::vector<Keys> &requests, const std::vector<Pcm> &pcms,
             SessionId &id, std::vector<Stream> &streams,
             Configure configure = nullptr, void *context = nullptr);
    int close(SessionId id);

private:
    int prepare_impl(SessionId id, Configure configure,
                      ConfigureParameters parameters, void *context);
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}

#endif
