// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/file_ipc.hpp"

#include "beamng_fsr/logging.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace bfsr {

FileIpc::FileIpc(std::string requestPath, std::string statusPath)
    : requestPath_(std::move(requestPath)), statusPath_(std::move(statusPath)) {}

bool FileIpc::readRequest(IpcRequest& out, std::string* diagnostic) {
    if (diagnostic) diagnostic->clear();

    std::ifstream in(requestPath_, std::ios::binary);
    if (!in) return false; // The mod half has not written anything yet.

    std::ostringstream ss;
    ss << in.rdbuf();

    std::string parseError;
    const Json j = Json::parse(ss.str(), &parseError);
    if (j.isNull()) {
        // Almost always a read that raced the mod half's rename. Silently wait
        // for the next tick rather than reverting to defaults.
        if (diagnostic) *diagnostic = "request document is not valid JSON: " + parseError;
        return false;
    }

    const IpcRequest request = IpcRequest::fromJson(j);
    if (!isProtocolCompatible(request.protocolVersion)) {
        if (!warnedAboutProtocol_) {
            warnedAboutProtocol_ = true;
            BFSR_LOG_ERROR("the BeamNG FSR mod speaks IPC protocol version %d but this bridge "
                           "speaks %d. Update both halves to the same release; the bridge is "
                           "ignoring its requests until then.",
                           request.protocolVersion, kIpcProtocolVersion);
        }
        if (diagnostic) *diagnostic = "incompatible IPC protocol version";
        return false;
    }

    if (request.sequence == lastSequence_) return false; // nothing new
    lastSequence_ = request.sequence;
    out = request;
    return true;
}

bool FileIpc::writeStatus(const IpcStatus& status) {
    IpcStatus copy = status;
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

} // namespace bfsr
