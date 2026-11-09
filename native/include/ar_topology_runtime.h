/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_RUNTIME_H
#define AR_TOPOLOGY_RUNTIME_H

#include "ar_topology_bindings.h"

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

class Runtime {
public:
    Runtime(unsigned card, struct mixer *mixer, const Bindings &bindings);
    ~Runtime();
    int open(const std::vector<Keys> &requests, const std::vector<Pcm> &pcms,
             SessionId &id, std::vector<Stream> &streams,
             Configure configure = nullptr, void *context = nullptr);
    int close(SessionId id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}

#endif
