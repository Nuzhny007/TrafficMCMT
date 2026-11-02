#include "trafficmcmt/core/Log.h"

#include <filesystem>
#include <iostream>
#include <system_error>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace tmcmt {
namespace {

spdlog::level::level_enum ParseLevel(const std::string& level) noexcept {
    if (level == "trace") {
        return spdlog::level::trace;
    }
    if (level == "debug") {
        return spdlog::level::debug;
    }
    if (level == "warn") {
        return spdlog::level::warn;
    }
    if (level == "error") {
        return spdlog::level::err;
    }
    if (level == "critical") {
        return spdlog::level::critical;
    }
    if (level == "off") {
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

bool IsValidLogLevel(const std::string& level) noexcept {
    return level == "trace" || level == "debug" || level == "info" || level == "warn" || level == "error" ||
           level == "critical" || level == "off";
}

std::shared_ptr<spdlog::logger> InitLogging(const LogConfig& config, const std::string& name) {
    std::vector<spdlog::sink_ptr> sinks;

    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    if (!config.file.empty()) {
        try {
            const std::filesystem::path filePath(config.file);
            const std::filesystem::path parent = filePath.parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    std::cerr << "TrafficMCMT: cannot create log directory '" << parent.string()
                              << "': " << ec.message() << '\n';
                }
            }
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.file, config.max_size_bytes, config.max_files));
        } catch (const spdlog::spdlog_ex& e) {
            std::cerr << "TrafficMCMT: cannot open log file '" << config.file << "': " << e.what() << '\n';
        }
    }

    if (sinks.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    logger->set_level(ParseLevel(config.level));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%t] [%l] %v");
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(logger);
    return logger;
}

} // namespace tmcmt
