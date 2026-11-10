/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_parameters.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <acdb.h>
#include <ar_osal_error.h>
#include <tinyalsa/asoundlib.h>
#include <sound/snd_ar_tokens.h>

namespace ar_topology
{
namespace
{

constexpr uint32_t vcpm_instance = 4;

struct Run {
    uint32_t graph;
    size_t begin, end;
};

int frontend_graph(const Plan &plan, uint32_t &graph)
{
    std::set<uint32_t> graphs;

    for (const auto &frontend : plan.frontends)
        graphs.insert(frontend.graph_id);

    if (graphs.size() != 1)
        return -EOPNOTSUPP;

    graph = *graphs.begin();

    return 0;
}

int records(const Plan &plan, const void *data, size_t size, size_t applied,
             const std::set<uint32_t> *scope, std::vector<Run> &runs)
{
    if (!data || !size || applied > size ||
        size > size_t(std::numeric_limits<int32_t>::max()) - 8)
        return -EINVAL;

    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t offset = 0;

    while (offset < size) {
        uint32_t header[4];

        if (size - offset < sizeof(header))
            return -EINVAL;

        std::memcpy(header, bytes + offset, sizeof(header));

        size_t remaining = size - offset - sizeof(header);
        if (header[2] > remaining)
            return -EINVAL;

        size_t payload = (size_t(header[2]) + 7) & ~size_t(7);
        if (payload > remaining)
            return -EINVAL;

        uint32_t graph;

        if (header[0] == vcpm_instance) {
            int status = frontend_graph(plan, graph);
            if (status)
                return status;
        } else {
            auto owner = plan.modules.find(header[0]);
            if (owner == plan.modules.end())
                return header[0] == 3 ? -EOPNOTSUPP : -ENOENT;

            graph = owner->second;
        }

        if (scope && !scope->count(graph))
            return -ENOENT;

        size_t end = offset + sizeof(header) + payload;
        if (offset < applied && applied < end)
            return -EINVAL;

        if (offset >= applied) {
            if (!runs.empty() && runs.back().graph == graph)
                runs.back().end = end;
            else
                runs.push_back({graph, offset, end});
        }

        offset = end;
    }

    return 0;
}

int acdb_error(int status)
{
    switch (status) {
    case AR_EOK:
        return 0;
    case AR_ENOTEXIST:
        return -ENOENT;
    case AR_EBADPARAM:
        return -EINVAL;
    case AR_ENOMEMORY:
        return -ENOMEM;
    default:
        return -EIO;
    }
}

}

int Parameters::set(const void *data, size_t size, size_t *applied) const
{
    try {
        std::vector<Run> runs;
        int status = records(plan, data, size, applied ? *applied : 0,
                              nullptr, runs);
        if (status)
            return status;

        struct Pending {
            struct mixer_ctl *control;
            std::vector<uint8_t> bytes;
            size_t end;
        };
        std::vector<Pending> pending;

        for (const auto &run : runs) {
            const auto &graph = plan.graphs.at(run.graph);
            auto *control = mixer_get_ctl_by_name(mixer,
                                                   graph.set_control.c_str());
            if (!control)
                return -ENOENT;

            if (mixer_ctl_get_type(control) != MIXER_CTL_TYPE_BYTE ||
                !mixer_ctl_is_access_tlv_rw(control))
                return -EINVAL;

            uint32_t header[] = {SND_SOC_AR_TLV_SET_PARAMS,
                                 uint32_t(run.end - run.begin)};
            Pending value{control, std::vector<uint8_t>(sizeof(header) +
                                                        header[1]), run.end};
            std::memcpy(value.bytes.data(), header, sizeof(header));
            std::memcpy(value.bytes.data() + sizeof(header),
                         static_cast<const uint8_t *>(data) + run.begin,
                         header[1]);
            pending.push_back(std::move(value));
        }

        for (const auto &value : pending) {
            int status = mixer_ctl_set_array(value.control, value.bytes.data(),
                                              value.bytes.size());
            if (status)
                return status == -1 ? -errno : status;

            if (applied)
                *applied = value.end;
        }

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

int Parameters::resolve_tag(const Keys &gkv, uint32_t tag, const Keys &tkv,
                            std::vector<uint8_t> &data) const
{
    try {
        std::set<uint32_t> scope;

        if (gkv.empty()) {
            for (const auto &[id, graph] : plan.graphs) {
                (void)graph;
                scope.insert(id);
            }
        } else {
            auto keys = gkv;
            std::sort(keys.begin(), keys.end());

            auto selection = plan.selections.find(keys);
            if (selection == plan.selections.end())
                return -ENOENT;

            scope = selection->second;
        }

        if (scope.empty())
            return -ENOENT;

        std::vector<uint32_t> subgraphs;

        for (uint32_t id : scope)
            subgraphs.push_back(plan.graphs.at(id).subgraph);

        std::set<uint32_t> keys;
        std::vector<AcdbKeyValuePair> pairs;

        for (const auto &[key, value] : tkv) {
            if (!keys.insert(key).second)
                return -EINVAL;

            pairs.push_back({key, value});
        }

        AcdbSgIdModuleTag request{};
        request.num_sg_ids = subgraphs.size();
        request.sg_ids = subgraphs.data();
        request.module_tag.tag_id = tag;
        request.module_tag.tag_key_vector.num_keys = pairs.size();
        request.module_tag.tag_key_vector.graph_key_vector = pairs.data();
        AcdbBlob response{};
        int status = acdb_error(acdb_ioctl(ACDB_CMD_GET_MODULE_TAG_DATA,
            &request, sizeof(request), &response, sizeof(response)));
        if (status)
            return status;

        if (response.buf_size >
            uint32_t(std::numeric_limits<int32_t>::max()) - 8)
            return -E2BIG;

        std::vector<uint8_t> result(response.buf_size);
        if (!result.empty()) {
            response.buf = result.data();
            status = acdb_error(acdb_ioctl(ACDB_CMD_GET_MODULE_TAG_DATA,
                &request, sizeof(request), &response, sizeof(response)));
            if (status)
                return status;

            if (response.buf_size > result.size())
                return -EIO;

            result.resize(response.buf_size);
            std::vector<Run> runs;

            if (!result.empty()) {
                status = records(plan, result.data(), result.size(), 0,
                                  &scope, runs);
                if (status)
                    return status;
            }
        }

        data = std::move(result);

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

}
