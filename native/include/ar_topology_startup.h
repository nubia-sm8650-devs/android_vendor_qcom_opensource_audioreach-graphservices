/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef AR_TOPOLOGY_STARTUP_H
#define AR_TOPOLOGY_STARTUP_H

#include "ar_topology_bindings.h"

#include <memory>
#include <string>

namespace ar_topology
{

constexpr const char *card_property = "ro.vendor.audio.native_topology.card";
constexpr const char *alias_property = "ro.vendor.audio.native_topology.alias";
constexpr const char *bindings_path =
    "/vendor/etc/audio/audioreach-native-bindings.json";

int find_card(const std::string &name, unsigned &card);
int load_bindings(const std::string &path, std::unique_ptr<Bindings> &bindings);

}

#endif
