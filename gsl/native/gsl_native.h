/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef GSL_NATIVE_H
#define GSL_NATIVE_H

#include <ar_topology_bindings.h>

#include <memory>

struct mixer;

namespace gsl_native
{

int start(unsigned card, struct mixer *mixer,
          std::unique_ptr<ar_topology::Bindings> bindings);
void stop();

}

#endif
