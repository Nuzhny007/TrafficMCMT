#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Log.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/pipeline/MasterPipeline.h"

#if defined(TMCMT_MASTER_WITH_IO)
#include "trafficmcmt/io/FileTrackletSource.h"
#include "trafficmcmt/io/ResultSinks.h"
#include "trafficmcmt/io/TrackletCodec.h"
#include "trafficmcmt/metrics/MotMetrics.h"
#endif

#if defined(TMCMT_MASTER_WITH_ZMQ)
#include "trafficmcmt/net/ZmqTrackletSource.h"
#endif

namespace {

using tmcmt::MasterConfig;
using tmcmt::PipelineMode;

constexpr int kExitOk = 0;
constexpr int kExitConfigError = 1;
constexpr int kExitUsageError = 2;
constexpr int kExitDataError = 3;
constexpr int kExitIoError = 4;

struct Options {
    std::string configPath = "configs/master.yaml";
    std::string logConfigPath;
    std::string logLevel;
    std::string resultsPath;
    std::string statsPath;
    std::string rawPath;
    std::string groundTruthPath;
    std::optional<PipelineMode> mode;
    std::optional<int> maxEvents;
    std::optional<int> idleTimeoutMs;
    bool dumpConfig = false;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT master node " << tmcmt::kVersion << " (protocol v" << tmcmt::kProtocolVersion
              << ")\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  --config <path>            Path to the master configuration (default: configs/master.yaml)\n"
              << "  --log-config <path>        Path to a standalone logging configuration\n"
              << "  --log-level <level>        Override log level: trace|debug|info|warn|error|critical|off\n"
              << "  --mode <mode>              Override pipeline mode: offline|online\n"
              << "  --out <path>               Write global track updates (JSONL) and merges to <path>.merges.jsonl\n"
              << "  --stats-out <path>         Write pipeline statistics as JSON\n"
              << "  --dump-raw <path>          Write raw/corrected timings as JSONL\n"
              << "  --gt <path>                Compare against a previous result JSONL (pseudo ground truth)\n"
              << "  --max-events <n>           Stop online after n ingested tracklets\n"
              << "  --stop-after-idle-ms <n>   Stop online after n ms without data\n"
              << "  --dump-config              Print the effective configuration and exit\n"
              << "  --version                  Print version and exit\n"
              << "  -h, --help                 Print this help and exit\n";
}

std::optional<int> ParseInt(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        if (consumed != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
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
        } else if (arg == "--out") {
            auto value = nextValue("--out");
            if (!value) {
                return std::nullopt;
            }
            options.resultsPath = std::move(*value);
        } else if (arg == "--stats-out") {
            auto value = nextValue("--stats-out");
            if (!value) {
                return std::nullopt;
            }
            options.statsPath = std::move(*value);
        } else if (arg == "--dump-raw") {
            auto value = nextValue("--dump-raw");
            if (!value) {
                return std::nullopt;
            }
            options.rawPath = std::move(*value);
        } else if (arg == "--gt") {
            auto value = nextValue("--gt");
            if (!value) {
                return std::nullopt;
            }
            options.groundTruthPath = std::move(*value);
        } else if (arg == "--max-events") {
            auto value = nextValue("--max-events");
            if (!value) {
                return std::nullopt;
            }
            const auto parsed = ParseInt(*value);
            if (!parsed || *parsed < 0) {
                error = "invalid value for --max-events: '" + *value + "'";
                return std::nullopt;
            }
            options.maxEvents = *parsed;
        } else if (arg == "--stop-after-idle-ms") {
            auto value = nextValue("--stop-after-idle-ms");
            if (!value) {
                return std::nullopt;
            }
            const auto parsed = ParseInt(*value);
            if (!parsed || *parsed < 0) {
                error = "invalid value for --stop-after-idle-ms: '" + *value + "'";
                return std::nullopt;
            }
            options.idleTimeoutMs = *parsed;
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

#if defined(TMCMT_MASTER_WITH_IO)

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char ch : text) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

int WriteStats(const std::string& path, const tmcmt::pipeline::PipelineStats& stats, const MasterConfig& config,
               const std::optional<tmcmt::metrics::MetricsReport>& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"mode\": \"" << tmcmt::ToString(config.mode) << "\",\n";
    out << "  \"ingested\": " << stats.counters.ingested << ",\n";
    out << "  \"accepted\": " << stats.counters.accepted << ",\n";
    out << "  \"late\": " << stats.counters.late << ",\n";
    out << "  \"too_late\": " << stats.counters.too_late << ",\n";
    out << "  \"duplicates\": " << stats.counters.duplicates << ",\n";
    out << "  \"dropped\": " << stats.counters.dropped << ",\n";
    out << "  \"windows_solved\": " << stats.counters.windows_solved << ",\n";
    out << "  \"windows_reopened\": " << stats.counters.windows_reopened << ",\n";
    out << "  \"windows_committed\": " << stats.counters.windows_committed << ",\n";
    out << "  \"results_published\": " << stats.counters.results_published << ",\n";
    out << "  \"merges_published\": " << stats.counters.merges_published << ",\n";
    out << "  \"sink_failures\": " << stats.counters.sink_failures << ",\n";
    out << "  \"watermark_ms\": " << stats.counters.watermark_ms << ",\n";
    out << "  \"buffered\": " << stats.buffered << ",\n";
    out << "  \"tracklets\": " << stats.tracklets << ",\n";
    out << "  \"gallery\": " << stats.gallery << ",\n";
    out << "  \"ingest_latency_ms\": {\"count\": " << stats.ingest_latency_ms.count
        << ", \"p50\": " << stats.ingest_latency_ms.p50_ms << ", \"p95\": " << stats.ingest_latency_ms.p95_ms
        << ", \"p99\": " << stats.ingest_latency_ms.p99_ms << ", \"max\": " << stats.ingest_latency_ms.max_ms
        << "},\n";
    out << "  \"lateness_ms\": {\"count\": " << stats.lateness_ms.count << ", \"p50\": " << stats.lateness_ms.p50_ms
        << ", \"p95\": " << stats.lateness_ms.p95_ms << ", \"max\": " << stats.lateness_ms.max_ms << "},\n";
    out << "  \"cameras\": [";
    for (std::size_t i = 0; i < stats.cameras.size(); ++i) {
        const auto& camera = stats.cameras[i];
        if (i != 0) {
            out << ", ";
        }
        out << "{\"camera\": \"" << JsonEscape(camera.camera) << "\", \"offset_ms\": " << camera.offset_ms
            << ", \"drift_ppm\": " << camera.drift_ppm << ", \"residual_ms\": " << camera.residual_ms
            << ", \"samples\": " << camera.samples << ", \"synced\": " << (camera.synced ? "true" : "false") << "}";
    }
    out << "]";
    if (report) {
        out << ",\n  \"metrics\": {\"gt_tracks\": " << report->gt_tracks
            << ", \"predicted_tracks\": " << report->predicted_tracks << ", \"idtp\": " << report->idtp
            << ", \"idfp\": " << report->idfp << ", \"idfn\": " << report->idfn << ", \"idsw\": " << report->idsw
            << ", \"idf1\": " << report->idf1 << ", \"idp\": " << report->idp << ", \"idr\": " << report->idr << "}";
    }
    out << "\n}\n";

    if (path.empty()) {
        std::cout << out.str();
        return kExitOk;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "error: cannot open stats file: " << path << "\n";
        return kExitIoError;
    }
    file << out.str();
    if (!file.good()) {
        std::cerr << "error: cannot write stats file: " << path << "\n";
        return kExitIoError;
    }
    return kExitOk;
}

int WriteRawTimings(const std::string& path, const tmcmt::pipeline::MasterPipeline& pipeline) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "error: cannot open raw timing file: " << path << "\n";
        return kExitIoError;
    }

    for (const auto& [id, timing] : pipeline.RawTimings()) {
        file << "{\"tracklet_id\": " << id << ", \"camera\": \"" << JsonEscape(timing.camera) << "\", \"raw_begin\": "
             << timing.raw_begin.count() << ", \"raw_end\": " << timing.raw_end.count()
             << ", \"received_at\": " << timing.received_at.count() << ", \"offset_ms\": " << timing.correction.offset_ms
             << ", \"drift_ppm\": " << timing.correction.drift_ppm
             << ", \"corrected_begin\": " << timing.correction.Apply(timing.raw_begin).count()
             << ", \"corrected_end\": " << timing.correction.Apply(timing.raw_end).count() << "}\n";
    }
    if (!file.good()) {
        std::cerr << "error: cannot write raw timing file: " << path << "\n";
        return kExitIoError;
    }
    return kExitOk;
}

tmcmt::Result<std::vector<tmcmt::metrics::TrackIdentity>> LoadGroundTruth(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return tmcmt::Status::Error(tmcmt::ErrorCode::IoError, "cannot open ground truth file: " + path);
    }

    std::vector<tmcmt::metrics::TrackIdentity> gt;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        if (line.empty()) {
            continue;
        }
        auto decoded = tmcmt::GlobalTrackCodec::DecodeJson(line);
        if (!decoded) {
            return tmcmt::Status::Error(tmcmt::ErrorCode::ParseError,
                                        path + ":" + std::to_string(lineNumber) + ": " +
                                            decoded.status().message());
        }
        const tmcmt::GlobalTrackUpdate& update = decoded.value();
        for (const tmcmt::Tracklet& segment : update.segments) {
            gt.push_back(tmcmt::metrics::TrackIdentity{segment.id, std::to_string(update.global_id), segment.begin});
        }
    }
    return gt;
}

int RunPipeline(MasterConfig config, const Options& options, const std::shared_ptr<spdlog::logger>& logger) {
    if (options.maxEvents) {
        config.online.max_events = *options.maxEvents;
    }
    if (options.idleTimeoutMs) {
        config.online.idle_timeout_ms = *options.idleTimeoutMs;
    }

    if (config.ingest.source == tmcmt::SourceKind::File && config.clock_sync.enabled) {
        logger->warn("file replay disables clock synchronization: receive time is not an edge clock");
        config.clock_sync.enabled = false;
    }

    tmcmt::pipeline::PipelineDependencies dependencies;
    dependencies.logger = logger;

    if (config.ingest.source == tmcmt::SourceKind::File) {
        tmcmt::FileTrackletSource::Options sourceOptions;
        sourceOptions.format = config.ingest.file.format;
        sourceOptions.speed = config.mode == PipelineMode::Offline ? 0.0 : config.ingest.file.speed;
        auto source = tmcmt::FileTrackletSource::Open(config.ingest.file.path, sourceOptions);
        if (!source) {
            std::cerr << "error: " << source.status().ToString() << "\n";
            return kExitDataError;
        }
        dependencies.source = std::move(source).value();
    } else {
#if defined(TMCMT_MASTER_WITH_ZMQ)
        tmcmt::net::ZmqSourceOptions zmqOptions;
        zmqOptions.endpoint = config.ingest.zmq.endpoint;
        zmqOptions.subscribe_prefix = config.ingest.zmq.subscribe_prefix;
        zmqOptions.rcv_hwm = config.ingest.zmq.rcv_hwm;
        zmqOptions.reconnect_ivl_ms = config.ingest.zmq.reconnect_ivl_ms;
        zmqOptions.normalize = !config.ingest.strict;
        zmqOptions.strict = config.ingest.strict;
        zmqOptions.bind = config.ingest.zmq.endpoint.find("0.0.0.0") != std::string::npos ||
                          config.ingest.zmq.endpoint.find("://*") != std::string::npos;

        auto source = std::make_unique<tmcmt::net::ZmqTrackletSource>();
        const tmcmt::Status started = source->Start(zmqOptions);
        if (!started.ok()) {
            std::cerr << "error: " << started.ToString() << "\n";
            return kExitConfigError;
        }
        dependencies.source = std::move(source);
#else
        std::cerr << "error: zmq ingest requires TRAFFICMCMT_WITH_ZMQ=ON\n";
        return kExitConfigError;
#endif
    }

    if (!options.resultsPath.empty()) {
        auto sink = tmcmt::JsonlResultSink::Open(options.resultsPath);
        if (!sink) {
            std::cerr << "error: " << sink.status().ToString() << "\n";
            return kExitIoError;
        }
        dependencies.sinks.push_back(std::move(sink).value());
    }

    auto created = tmcmt::pipeline::MasterPipeline::Create(config, std::move(dependencies));
    if (!created) {
        std::cerr << "error: " << created.status().ToString() << "\n";
        return kExitConfigError;
    }
    std::unique_ptr<tmcmt::pipeline::MasterPipeline> pipeline = std::move(created).value();

    const tmcmt::Status status =
        config.mode == PipelineMode::Online ? pipeline->RunOnline() : pipeline->RunOffline();
    if (!status.ok()) {
        std::cerr << "error: " << status.ToString() << "\n";
        return kExitDataError;
    }

    if (!options.rawPath.empty()) {
        const int code = WriteRawTimings(options.rawPath, *pipeline);
        if (code != kExitOk) {
            return code;
        }
    }

    std::optional<tmcmt::metrics::MetricsReport> report;
    if (!options.groundTruthPath.empty()) {
        auto groundTruth = LoadGroundTruth(options.groundTruthPath);
        if (!groundTruth) {
            std::cerr << "error: " << groundTruth.status().ToString() << "\n";
            return kExitDataError;
        }

        std::vector<tmcmt::metrics::PredictedIdentity> predicted;
        for (const auto& [trackletId, globalId] : pipeline->Assignments()) {
            predicted.push_back(tmcmt::metrics::PredictedIdentity{trackletId, globalId});
        }
        report = tmcmt::metrics::MotMetrics::Compute(groundTruth.value(), predicted);
    }

    const tmcmt::pipeline::PipelineStats stats = pipeline->Stats();
    const int statsCode = WriteStats(options.statsPath, stats, config, report);
    if (statsCode != kExitOk) {
        return statsCode;
    }

    logger->info("done: ingested={} accepted={} late={} too_late={} windows_solved={} windows_committed={}",
                 stats.counters.ingested, stats.counters.accepted, stats.counters.late, stats.counters.too_late,
                 stats.counters.windows_solved, stats.counters.windows_committed);
    if (report) {
        logger->info("metrics: idf1={:.4f} idp={:.4f} idr={:.4f} idsw={} gt_tracks={} predicted_tracks={}",
                     report->idf1, report->idp, report->idr, report->idsw, report->gt_tracks,
                     report->predicted_tracks);
    }
    return kExitOk;
}

#endif // TMCMT_MASTER_WITH_IO

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
    logger->info("clock_sync: enabled={} min_samples={} watermark.silent_timeout_ms={} online.clock={}",
                 config.clock_sync.enabled, config.clock_sync.min_samples, config.watermark.silent_timeout_ms,
                 tmcmt::ToString(config.online.clock));

#if defined(TMCMT_MASTER_WITH_IO)
    return RunPipeline(std::move(config), *options, logger);
#else
    logger->warn("pipeline is not wired: build with TRAFFICMCMT_WITH_PROTOBUF=ON to enable ingest and sinks");
    logger->info("configuration and logging are operational");
    return kExitOk;
#endif
}
