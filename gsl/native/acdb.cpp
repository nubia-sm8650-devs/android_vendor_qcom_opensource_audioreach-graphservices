/* SPDX-License-Identifier: BSD-3-Clause */
#define LOG_TAG "gsl-native"

#include <ar_topology_log.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <acdb.h>
#include <ar_osal_error.h>
#include <gsl_intf.h>

namespace
{

AcdbGraphKeyVector vector(const gsl_key_vector *keys)
{
    AcdbGraphKeyVector value{};

    if (keys) {
        value.num_keys = keys->num_kvps;
        value.graph_key_vector = reinterpret_cast<AcdbKeyValuePair *>(keys->kvp);
    }

    return value;
}

std::string text(const gsl_key_vector *keys)
{
    ar_topology::Keys value;

    for (uint32_t i = 0; keys && i < keys->num_kvps; i++)
        value.emplace_back(keys->kvp[i].key, keys->kvp[i].value);

    return ar_topology::text(value);
}

int32_t subgraphs(const gsl_key_vector *keys, std::vector<uint32_t> &ids)
{
    AcdbGraphKeyVector request = vector(keys);
    AcdbGetGraphRsp reply{};

    int32_t status = acdb_ioctl(ACDB_CMD_GET_GRAPH, &request, sizeof(request),
                                &reply, sizeof(reply));
    if (status)
        ALOGW("the ACDB has no graph for %s, status %d", text(keys).c_str(),
              status);

    if (status || !reply.size)
        return status;

    std::vector<uint32_t> words((reply.size + 3) / 4);

    reply.subgraphs = reinterpret_cast<AcdbSubgraph *>(words.data());

    status = acdb_ioctl(ACDB_CMD_GET_GRAPH, &request, sizeof(request), &reply,
                        sizeof(reply));
    if (status)
        return status;

    size_t count = reply.size / 4;

    for (size_t i = 0; i < count;) {
        if (count - i < 2 || words[i + 1] > count - i - 2)
            return AR_EFAILED;

        ids.push_back(words[i]);
        i += 2 + words[i + 1];
    }

    return AR_EOK;
}

}

extern "C" int32_t gsl_get_tags_with_module_info(
    const struct gsl_key_vector *graph_key_vect, void *tag_module_info,
    size_t *tag_module_info_size)
{
    if (!graph_key_vect || !tag_module_info_size)
        return AR_EBADPARAM;

    AcdbGraphKeyVector keys = vector(graph_key_vect);
    AcdbCmdGetTagsFromGkvReq request{};
    AcdbCmdGetTagsFromGkvRsp reply{};
    auto *info = static_cast<gsl_tag_module_info *>(tag_module_info);

    request.graph_key_vector = &keys;

    if (info && *tag_module_info_size >= sizeof(*info)) {
        reply.list_size = uint32_t(*tag_module_info_size - sizeof(*info));
        reply.tag_module_list =
            reinterpret_cast<AcdbTagModule *>(info->tag_module_entry);
    } else {
        info = nullptr;
    }

    int32_t status = acdb_ioctl(ACDB_CMD_GET_TAGS_FROM_GKV, &request,
                                sizeof(request), &reply, sizeof(reply));
    if (status)
        return status;

    if (info)
        info->num_tags = reply.num_tags;

    *tag_module_info_size = reply.list_size + sizeof(gsl_tag_module_info);

    return AR_EOK;
}

extern "C" int32_t gsl_get_tagged_module_info(
    const struct gsl_key_vector *graph_key_vect, uint32_t tag,
    struct gsl_module_id_info **module_info, uint32_t *module_info_size)
{
    if (!graph_key_vect || !module_info || !module_info_size)
        return AR_EBADPARAM;

    *module_info = nullptr;
    *module_info_size = 0;

    try {
        std::vector<uint32_t> ids;

        int32_t status = subgraphs(graph_key_vect, ids);
        if (status)
            return status;

        if (ids.empty())
            return AR_ENOTEXIST;

        AcdbGetTaggedModulesReq request{};
        AcdbGetTaggedModulesRsp reply{};

        request.num_sg_ids = uint32_t(ids.size());
        request.sg_ids = ids.data();
        request.tag_id = tag;

        status = acdb_ioctl(ACDB_CMD_GET_TAGGED_MODULES, &request,
                            sizeof(request), &reply, sizeof(reply));
        if (status && status != AR_ENOTEXIST)
            return status;

        if (!reply.num_tagged_mids) {
            ALOGD("tag %#x has no module in %s", tag,
                  text(graph_key_vect).c_str());

            return AR_ENOTEXIST;
        }

        uint32_t size = sizeof(gsl_module_id_info) +
            reply.num_tagged_mids * sizeof(gsl_module_id_info_entry);
        auto *info = static_cast<gsl_module_id_info *>(calloc(1, size));

        if (!info)
            return AR_ENOMEMORY;

        info->num_modules = reply.num_tagged_mids;
        reply.tagged_mid_list =
            reinterpret_cast<AcdbModuleInstance *>(info->module_entry);

        status = acdb_ioctl(ACDB_CMD_GET_TAGGED_MODULES, &request,
                            sizeof(request), &reply, sizeof(reply));
        if (status) {
            free(info);

            return status;
        }

        *module_info = info;
        *module_info_size = size;

        ALOGD("tag %#x is module %#x instance %#x in %s", tag,
              info->module_entry[0].module_id, info->module_entry[0].module_iid,
              text(graph_key_vect).c_str());

        return AR_EOK;
    } catch (const std::bad_alloc &) {
        return AR_ENOMEMORY;
    }
}

extern "C" int32_t gsl_get_tagged_data(
    const struct gsl_key_vector *graph_key_vect, uint32_t tag,
    struct gsl_key_vector *tag_key_vect, uint8_t *payload,
    size_t *payload_size)
{
    if (!graph_key_vect || !payload_size)
        return AR_EBADPARAM;

    try {
        std::vector<uint32_t> ids;

        int32_t status = subgraphs(graph_key_vect, ids);
        if (status)
            return status;

        if (ids.empty()) {
            *payload_size = 0;

            return AR_EOK;
        }

        AcdbSgIdModuleTag request{};
        AcdbBlob reply{};

        request.num_sg_ids = uint32_t(ids.size());
        request.sg_ids = ids.data();
        request.module_tag.tag_id = tag;
        request.module_tag.tag_key_vector = vector(tag_key_vect);

        status = acdb_ioctl(ACDB_CMD_GET_MODULE_TAG_DATA, &request,
                            sizeof(request), &reply, sizeof(reply));
        if (status)
            return status;

        if (payload && *payload_size < reply.buf_size) {
            status = AR_ENEEDMORE;
        } else if (payload) {
            reply.buf = payload;

            status = acdb_ioctl(ACDB_CMD_GET_MODULE_TAG_DATA, &request,
                                sizeof(request), &reply, sizeof(reply));
        }

        *payload_size = reply.buf_size;

        return status;
    } catch (const std::bad_alloc &) {
        return AR_ENOMEMORY;
    }
}

extern "C" int32_t gsl_enable_acdb_persistence(uint8_t enable_flag)
{
    return acdb_ioctl(ACDB_CMD_ENABLE_PERSISTANCE, &enable_flag,
                      sizeof(enable_flag), nullptr, 0);
}

extern "C" int32_t gsl_set_cal_data_to_acdb(
    const struct gsl_key_vector *graph_key_vect,
    const struct gsl_key_vector *cal_key_vect, uint8_t *payload,
    uint32_t payload_size)
{
    if (!graph_key_vect || !cal_key_vect)
        return AR_EBADPARAM;

    AcdbSetCalDataReq request{};

    request.graph_key_vector = vector(graph_key_vect);
    request.cal_key_vector = vector(cal_key_vect);
    request.cal_blob_size = payload_size;
    request.cal_blob = payload;

    return acdb_ioctl(ACDB_CMD_SET_CAL_DATA, &request, sizeof(request),
                      nullptr, 0);
}

extern "C" int32_t gsl_get_cal_data_from_acdb(
    const struct gsl_key_vector *graph_key_vect,
    const struct gsl_key_vector *cal_key_vect, uint32_t num_modules,
    uint8_t *param_list, void *payload, uint32_t *payload_size)
{
    if (!graph_key_vect || !cal_key_vect || !payload_size)
        return AR_EBADPARAM;

    AcdbGetCalDataReq request{};
    AcdbBlob reply{};

    request.graph_key_vector = vector(graph_key_vect);
    request.cal_key_vector = vector(cal_key_vect);
    request.num_module_instance = num_modules;
    request.instance_param_list =
        reinterpret_cast<AcdbModuleParamList *>(param_list);

    if (payload) {
        reply.buf = payload;
        reply.buf_size = *payload_size;
    }

    int32_t status = acdb_ioctl(ACDB_CMD_GET_CAL_DATA, &request,
                                sizeof(request), &reply, sizeof(reply));

    *payload_size = reply.buf_size;

    return status;
}

extern "C" int32_t gsl_set_tag_data_to_acdb(
    const struct gsl_key_vector *graph_key_vect, uint32_t tag_id,
    const struct gsl_key_vector *tag_key_vect, uint8_t *payload,
    uint32_t payload_size)
{
    if (!graph_key_vect || !tag_key_vect)
        return AR_EBADPARAM;

    AcdbSetTagDataReq request{};

    request.graph_key_vector = vector(graph_key_vect);
    request.module_tag.tag_id = tag_id;
    request.module_tag.tag_key_vector = vector(tag_key_vect);
    request.blob_size = payload_size;
    request.blob = payload;

    return acdb_ioctl(ACDB_CMD_SET_TAG_DATA, &request, sizeof(request),
                      nullptr, 0);
}

extern "C" int32_t gsl_get_tag_data_from_acdb(
    const struct gsl_key_vector *graph_key_vect, uint32_t tag_id,
    const struct gsl_key_vector *tag_key_vect, uint32_t num_modules,
    uint8_t *param_list, void *payload, uint32_t *payload_size)
{
    if (!graph_key_vect || !tag_key_vect || !payload_size)
        return AR_EBADPARAM;

    AcdbGetTagDataReq request{};
    AcdbBlob reply{};

    request.graph_key_vector = vector(graph_key_vect);
    request.module_tag.tag_id = tag_id;
    request.module_tag.tag_key_vector = vector(tag_key_vect);
    request.num_module_instance = num_modules;
    request.instance_param_list =
        reinterpret_cast<AcdbModuleParamList *>(param_list);

    if (payload) {
        reply.buf = payload;
        reply.buf_size = *payload_size;
    }

    int32_t status = acdb_ioctl(ACDB_CMD_GET_TAG_DATA, &request,
                                sizeof(request), &reply, sizeof(reply));

    *payload_size = reply.buf_size;

    return status;
}

extern "C" int32_t gsl_get_graph_alias(
    const struct gsl_key_vector *graph_key_vect, char *alias,
    uint32_t *alias_len)
{
    if (!graph_key_vect || !alias_len)
        return AR_EBADPARAM;

    AcdbGraphKeyVector request = vector(graph_key_vect);
    AcdbString reply{};

    reply.length = *alias_len;
    reply.string = alias;

    int32_t status = acdb_ioctl(ACDB_CMD_GET_GRAPH_ALIAS, &request,
                                sizeof(request), &reply, sizeof(reply));

    *alias_len = reply.length;

    return status;
}
