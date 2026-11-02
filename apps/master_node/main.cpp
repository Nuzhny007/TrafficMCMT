#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Log.h"
#include "trafficmcmt/core/Version.h"

namespace {

using tmcmt::MasterConfig;
using tmcmt::PipelineMode;

constexpr int kExitOk = 0;
constexpr int kExitConfigError = 1;
constexpr int kExitUsageError = 2;

struct Options {
    std::string configPath = "configs/master.yaml";
    std::string logConfigPath;
    std::string logLevel;
    std::optional<PipelineMode> mode;
    bool dumpConfig = false;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT master node " << tmcmt::kVersion << " (protocol v" << tmcmt::kProtocolVersion
              << ")\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  --config <path>      Path to the master configuration (default: configs/master.yaml)\n"
              << "  --log-config <path>  Path to a standalone logging configuration\n"
              << "  --log-level <level>  Override log level: trace|debug|info|warn|error|critical|off\n"
              << "  --mode <mode>        Override pipeline mode: offline|online\n"
              << "  --dump-config        Print the effective configuration and exit\n"
              << "  --version            Print version and exit\n"
              << "  -h, --help           Print this help and exit\n";
}

std::optional<Options> ParseArgs(int argc, char** argv, std::string& error) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto nextValue = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                error = std::string("missing value for ") + name;
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        } else if (arg == "--version") {
            options.showVersion = true;
        } else if (arg == "--dump-config") {
            options.dumpConfig = true;
        } else if (arg == "--config") {
            auto value = nextValue("--config");
            if (!value) {
                return std::nullopt;
            }
            options.configPath = std::move(*value);
        } else if (arg == "--log-config") {
            auto value = nextValue("--log-config");
            if (!value) {
                return std::nullopt;
            }
            options.logConfigPath = std::move(*value);
        } else if (arg == "--log-level") {
            auto value = nextValue("--log-level");
            if (!value) {
                return std::nullopt;
            }
            options.logLevel = std::move(*value);
        } else if (arg == "--mode") {
            auto value = nextValue("--mode");
            if (!value) {
                return std::nullopt;
            }
            const auto parsed = tmcmt::ParsePipelineMode(*value);
            if (!parsed) {
                error = "unknown mode '" + *value + "', expected one of: offline|online";
                return std::nullopt;
            }
            options.mode = *parsed;
        } else {
            error = "unknown argument '" + arg + "'";
            return std::nullopt;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    std::string parseError;
    std::optional<Options> options = ParseArgs(argc, argv, parseError);
    if (!options) {
        std::cerr << "error: " << parseError << "\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_master");
        return kExitUsageError;
    }

    if (options->showHelp) {
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_master");
        return kExitOk;
    }
    if (options->showVersion) {
        std::cout << "TrafficMCMT " << tmcmt::kVersion << " (protocol v" << tmcmt::kProtocolVersion << ")\n";
        return kExitOk;
    }

    auto configResult = MasterConfig::Load(options->configPath);
    if (!configResult) {
        std::cerr << "error: " << configResult.status().ToString() << '\n';
        return kExitConfigError;
    }
    MasterConfig config = std::move(configResult).value();

    if (!options->logConfigPath.empty()) {
        auto logResult = tmcmt::LoadLogConfig(options->logConfigPath);
        if (!logResult) {
            std::cerr << "error: " << logResult.status().ToString() << '\n';
            return kExitConfigError;
        }
        config.log = std::move(logResult).value();
    }
    if (!options->logLevel.empty()) {
        config.log.level = options->logLevel;
    }
    if (options->mode) {
        config.mode = *options->mode;
    }

    const tmcmt::Status validation = config.Validate();
    if (!validation.ok()) {
        std::cerr << "error: " << validation.ToString() << '\n';
        return kExitConfigError;
    }

    if (options->dumpConfig) {
        std::cout << tmcmt::Dump(config);
        return kExitOk;
    }

    const std::shared_ptr<spdlog::logger> logger = tmcmt::InitLogging(config.log);
    logger->info("TrafficMCMT {} starting (protocol v{})", tmcmt::kVersion, tmcmt::kProtocolVersion);
    logger->info("mode={} window_ms={} overlap_ms={} commit_lag_ms={}", tmcmt::ToString(config.mode),
                 config.window_ms, config.window_overlap_ms, config.commit_lag_ms);

    if (config.ingest.source == tmcmt::SourceKind::File) {
        logger->info("ingest: file '{}' (speed x{})", config.ingest.file.path, config.ingest.file.speed);
    } else {
        logger->info("ingest: zmq '{}' (prefix '{}')", config.ingest.zmq.endpoint,
                     config.ingest.zmq.subscribe_prefix);
    }
    logger->info("reid: index={} dim={} top_k={}", tmcmt::ToString(config.reid.index), config.reid.embedding_dim,
                 config.reid.top_k);
    logger->info("st: gate={} weights=(app={}, time={}, space={})", tmcmt::ToString(config.st.gate.mode),
                 config.st.weights.appearance, config.st.weights.temporal, config.st.weights.spatial);
    logger->info("flow: solver={} candidates_per_node={}", tmcmt::ToString(config.flow.solver),
                 config.flow.max_candidates_per_node);

    // Stage 1 stops here: the pipeline itself is assembled in stages 2-7.
    logger->warn("pipeline is not wired yet: stages 2-7 add ingest, reid, st and flow");
    logger->info("stage 1 checks passed: configuration and logging are operational");
    return kExitOk;
}
