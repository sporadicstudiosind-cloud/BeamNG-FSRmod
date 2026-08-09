// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/result.hpp"

namespace bfsr {

const char* toString(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NotInitialised: return "not_initialised";
        case Status::Unsupported: return "unsupported";
        case Status::MissingDependency: return "missing_dependency";
        case Status::InvalidArgument: return "invalid_argument";
        case Status::RuntimeError: return "runtime_error";
        case Status::DeviceLost: return "device_lost";
        case Status::Disabled: return "disabled";
    }
    return "unknown";
}

} // namespace bfsr
