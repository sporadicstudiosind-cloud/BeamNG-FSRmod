// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <string>
#include <utility>

namespace bfsr {

/// Coarse failure classification. Kept small and stable: it is serialised into
/// the IPC status document and shown in the in-game diagnostics panel, so the
/// numeric values must not be reshuffled between releases.
enum class Status : int {
    Ok = 0,
    NotInitialised = 1,
    Unsupported = 2,       ///< Hardware/API/OS genuinely cannot do this.
    MissingDependency = 3, ///< An AMD DLL or system component is absent.
    InvalidArgument = 4,
    RuntimeError = 5,      ///< The underlying runtime (D3D12/FFX) reported failure.
    DeviceLost = 6,
    Disabled = 7,          ///< Switched off deliberately (safe mode, user, policy).
};

const char* toString(Status s) noexcept;

/// Result carrying a human-readable reason. The reason string is what ends up
/// in the log and in the UI, so it should read as an explanation, not a code.
class Result {
public:
    Result() noexcept = default;
    Result(Status s, std::string reason) : status_(s), reason_(std::move(reason)) {}

    static Result ok() noexcept { return Result{}; }
    static Result fail(Status s, std::string reason) { return Result{s, std::move(reason)}; }

    [[nodiscard]] bool isOk() const noexcept { return status_ == Status::Ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return isOk(); }
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

    /// Prefixes the reason with context while preserving the status, so nested
    /// failures read as "FSR upscaler: FFX context creation: <original>".
    [[nodiscard]] Result withContext(const std::string& ctx) const {
        if (isOk()) return *this;
        return Result{status_, ctx + ": " + reason_};
    }

private:
    Status status_ = Status::Ok;
    std::string reason_;
};

} // namespace bfsr
