/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_PARAMETERS_H
#define AR_TOPOLOGY_PARAMETERS_H

#include "ar_topology_bindings.h"

#include <cstddef>

namespace ar_topology
{

class Parameters {
public:
    Parameters(const Parameters &) = delete;
    Parameters &operator=(const Parameters &) = delete;
    int set(const void *data, size_t size, size_t *applied = nullptr) const;
    int get(void *data, size_t size) const;
    int resolve_tag(const Keys &gkv, uint32_t tag, const Keys &tkv,
                     std::vector<uint8_t> &data) const;

private:
    friend class Runtime;
    Parameters(struct mixer *mixer, const Plan &plan)
        : mixer(mixer), plan(plan)
    {
    }

    struct mixer *mixer;
    const Plan &plan;
};

}

#endif
