// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <string>

#include "beamng_fsr/ipc_protocol.hpp"

namespace bfsr {

/// One file-based IPC channel, used once per product.
///
/// Files rather than a pipe or socket because BeamNG's Lua sandbox permits file
/// I/O and nothing else — no ffi.load, no sockets. Nothing latency-critical
/// travels this way: the per-frame render data is GPU-side, and every field in
/// the protocol changes on the order of seconds.
///
/// Both directions write via a temporary file and rename, so a reader never
/// observes a half-written document. A torn read would otherwise parse as
/// "settings reset to defaults", the worst possible failure for a settings
/// channel.
template <typename RequestT, typename StatusT>
class FileIpcChannel {
public:
    FileIpcChannel(std::string requestPath, std::string statusPath)
        : requestPath_(std::move(requestPath)), statusPath_(std::move(statusPath)) {}

    /// False when there is no new request, or the document is unreadable or
    /// speaks an incompatible protocol version.
    bool readRequest(RequestT& out, std::string* diagnostic = nullptr);
    bool writeStatus(const StatusT& status);

    [[nodiscard]] uint64_t lastSequence() const noexcept { return lastSequence_; }
    [[nodiscard]] bool sawProtocolMismatch() const noexcept { return protocolMismatch_; }
    [[nodiscard]] int mismatchedVersion() const noexcept { return mismatchedVersion_; }

private:
    std::string requestPath_;
    std::string statusPath_;
    uint64_t lastSequence_ = 0;
    uint64_t statusSequence_ = 0;
    bool protocolMismatch_ = false;
    int mismatchedVersion_ = 0;
};

using FrameGenIpc = FileIpcChannel<FrameGenRequest, FrameGenStatus>;
using UpscaleIpc = FileIpcChannel<UpscaleRequest, UpscaleStatus>;

extern template class FileIpcChannel<FrameGenRequest, FrameGenStatus>;
extern template class FileIpcChannel<UpscaleRequest, UpscaleStatus>;

} // namespace bfsr
