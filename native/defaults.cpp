/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology.h"

#include <cerrno>
#include <limits>
#include <new>
#include <acdb.h>
#include <ar_osal_error.h>

namespace ar_topology
{

int read_defaults(uint32_t subgraph, std::vector<uint8_t> &data)
{
    data.clear();
    AcdbSgIdCalKeyVector request{};
    request.num_sg_ids = 1;
    request.sg_ids = &subgraph;
    AcdbGlbPsistIdentifierList global{};
    int status = acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_GLB_PSIST_IDENTIFIERS,
                            &request, sizeof(request), &global, sizeof(global));
    if (status && status != AR_ENOTEXIST)
        return -EIO;

    if (!status && (global.size || global.num_glb_persist_identifiers))
        return -EOPNOTSUPP;

    AcdbSgIdPersistCalData persistent{};
    status = acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_PERSIST,
                        &request, sizeof(request), &persistent,
                        sizeof(persistent));
    if (status && status != AR_ENOTEXIST)
        return -EIO;

    if (!status && persistent.cal_data_size > sizeof(AcdbSgIdPersistData))
        return -EOPNOTSUPP;

    AcdbBlob response{};
    status = acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_NONPERSIST,
                        &request, sizeof(request), &response, sizeof(response));
    if (status == AR_ENOTEXIST)
        return 0;

    if (status)
        return -EIO;

    if (!response.buf_size)
        return 0;

    if (response.buf_size > uint32_t(std::numeric_limits<int32_t>::max()) - 8)
        return -E2BIG;

    try {
        data.resize(response.buf_size);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }

    response.buf = data.data();
    status = acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_NONPERSIST,
                        &request, sizeof(request), &response, sizeof(response));
    if (status || response.buf_size > data.size()) {
        data.clear();
        return -EIO;
    }

    data.resize(response.buf_size);

    return 0;
}

}
