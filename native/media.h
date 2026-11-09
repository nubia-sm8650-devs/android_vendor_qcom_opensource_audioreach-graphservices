/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_MEDIA_INTERNAL_H
#define AR_TOPOLOGY_MEDIA_INTERNAL_H

#include "ar_topology_media.h"

struct mixer;
struct mixer_ctl;

namespace ar_topology
{

struct MediaWrite {
    struct mixer_ctl *control;
    unsigned item;
};

int media_writes(struct mixer *mixer, const Backend &backend,
                   const Media &media, std::vector<MediaWrite> &writes);

}

#endif
