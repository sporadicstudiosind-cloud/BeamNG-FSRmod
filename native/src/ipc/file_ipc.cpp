// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/file_ipc.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace bfsr {

template <typename RequestT, typename StatusT>
bool FileIpcChannel<RequestT, StatusT>::readRequest(RequestT& out, std::string* diagnostic) {
    if (diagnostic) diagnostic->clear();

    std::ifstream in(requestPath_, std::ios::binary);
    if (!in) return false; // This product's mod half has not written anything.

    std::ostringstream ss;
    ss << in.rdbuf();

    std::string parseError;
    const Json j = Json::parse(ss.str(), &parseError);
    if (j.isNull()) {
        // Almost always a read that raced the mod half's rename. Wait for the
        // next tick rather than reverting to defaults.
        if (diagnostic) *diagnostic = "request document is not valid JSON: " + parseError;
        return false;
    }

    const RequestT request = RequestT::fromJson(j);
    if (!isProtocolCompatible(request.protocolVersion)) {
        protocolMismatch_ = true;
        mismatchedVersion_ = request.protocolVersion;
        if (diagnostic) *diagnostic = "incompatible IPC protocol version";
        return false;
    }
    protocolMismatch_ = false;

    if (request.sequence == lastSequence_) return false; // nothing new
    lastSequence_ = request.sequence;
    out = request;
    return true;
}

template <typename RequestT, typename StatusT>
bool FileIpcChannel<RequestT, StatusT>::writeStatus(const StatusT& status) {
    StatusT copy = status;
    copy.sequence = ++statusSequence_;

    const std::string tmp = statusPath_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << copy.toJson().dump(1) << "\n";
        if (!out) return false;
    }
    std::remove(statusPath_.c_str());
    if (std::rename(tmp.c_str(), statusPath_.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

template class FileIpcChannel<FrameGenRequest, FrameGenStatus>;
template class FileIpcChannel<UpscaleRequest, UpscaleStatus>;

} // namespace bfsr
