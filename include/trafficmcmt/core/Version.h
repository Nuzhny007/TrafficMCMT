#pragma once

#ifndef TMCMT_VERSION
#define TMCMT_VERSION "0.0.0"
#endif

namespace tmcmt {

/// Library version, injected by CMake.
inline constexpr const char* kVersion = TMCMT_VERSION;

/// Wire protocol version (see docs/architecture.md, section 7).
inline constexpr unsigned kProtocolVersion = 1;

} // namespace tmcmt
