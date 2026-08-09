// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <string>

#include "beamng_fsr/ipc_protocol.hpp"

namespace bfsr {

/// File-based IPC between the BeamNG mod half and the bridge.
///
/// Files rather than a pipe or socket because BeamNG's Lua sandbox permits file
/// I/O and nothing else — no ffi.load, no sockets. Nothing latency-critical
/// travels this way: the per-frame render data is GPU-side, and every field in
/// the protocol changes on the order of seconds.
///
/// Both directions write via a temporary file and rename, so a reader never
/// observes a half-written document; a torn read would otherwise be parsed as
/// "settings reset to defaults", which is the worst possible failure mode for a
/// settings channel.
class FileIpc {
public:
    FileIpc(std::string requestPath, std::string statusPath);

    /// Returns false when there is no new request, or when the document is
    /// unreadable or speaks an incompatible protocol version.
    bool readRequest(IpcRequest& out, std::string* diagnostic = nullptr);

    bool writeStatus(const IpcStatus& status);

    [[nodiscard]] uint64_t lastSequence() const noexcept { return lastSequence_; }

private:
    std::string requestPath_;
    std::string statusPath_;
    uint64_t lastSequence_ = 0;
    uint64_t statusSequence_ = 0;
    bool warnedAboutProtocol_ = false;
};

} // namespace bfsr
