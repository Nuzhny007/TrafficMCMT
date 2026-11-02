#include "trafficmcmt/core/Result.h"

namespace tmcmt {

const char* ToString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "ok";
    case ErrorCode::InvalidArgument:
        return "invalid_argument";
    case ErrorCode::NotFound:
        return "not_found";
    case ErrorCode::IoError:
        return "io_error";
    case ErrorCode::ParseError:
        return "parse_error";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::Internal:
        return "internal";
    }
    return "internal";
}

std::string Status::ToString() const {
    if (ok()) {
        return "ok";
    }
    return std::string(tmcmt::ToString(m_code)) + ": " + m_message;
}

} // namespace tmcmt
