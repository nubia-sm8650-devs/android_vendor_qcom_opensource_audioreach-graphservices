/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef GSL_NATIVE_H
#define GSL_NATIVE_H

#include <ar_topology_bindings.h>

#include <chrono>
#include <memory>
#include <string>

struct gsl_init_data;
struct mixer;

namespace gsl_native
{

constexpr std::chrono::seconds card_wait{20};

using CardLost = void (*)();

int init(const gsl_init_data &data, const std::string &card,
         const std::string &bindings, std::chrono::milliseconds wait,
         CardLost lost);
void deinit();
int start(unsigned card, struct mixer *mixer,
          std::unique_ptr<ar_topology::Bindings> bindings);
void stop();

}

#endif
