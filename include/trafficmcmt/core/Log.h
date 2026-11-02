#pragma once

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "trafficmcmt/core/Config.h"

namespace tmcmt {

/// Returns true if `level` is one of trace/debug/info/warn/error/critical/off.
[[nodiscard]] bool IsValidLogLevel(const std::string& level) noexcept;

/// Creates the process-wide logger (console + rotating file) and installs it as
/// the spdlog default logger. Throws spdlog exceptions only on I/O failures.
[[nodiscard]] std::shared_ptr<spdlog::logger> InitLogging(const LogConfig& config,
                                                          const std::string& name = "tmcmt");

} // namespace tmcmt
