/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_PCM_CONTROLS_H
#define AR_TOPOLOGY_PCM_CONTROLS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ar_topology
{

int register_pcm_events(unsigned card, uint32_t pcm, bool capture,
                         const void *data, size_t size);
int read_pcm_event(unsigned card, uint32_t pcm, bool capture,
                     std::vector<uint8_t> &data);
int subscribe_pcm_events(unsigned card, uint32_t pcm, bool capture, int &fd);
int consume_pcm_notifications(int fd);

}

#endif
