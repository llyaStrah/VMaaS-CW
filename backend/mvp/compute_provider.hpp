#pragma once

#include "vmaas/ports/service_ports.hpp"
#include <memory>
#include <string>

namespace vmaas::mvp {
std::unique_ptr<ports::ComputeProvider> make_compute_provider(const std::string& provider_name);
}
