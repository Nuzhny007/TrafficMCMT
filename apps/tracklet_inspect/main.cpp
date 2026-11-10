#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/io/FileTrackletSource.h"
#include "trafficmcmt/io/TrackletCodec.h"
#include "trafficmcmt/net/ZmqTrackletSource.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitDataError = 3;

std::atomic<bool> g_stop{false};

void HandleSignal(int) {
    g_stop.store(true);
}

struct Options {
    std::string path;
    std::string source = "file";
    tmcmt::TrackletFileFormat format = tmcmt::TrackletFileFormat::Auto;
    std::size_t limit = 0;
    bool printJson = false;
    bool pretty = false;
    std::string endpoint = "tcp://127.0.0.1:5556";
    std::string subscribePrefix = "tmcmt.v1.";
    int timeoutMs = 1'000;
    int durationSeconds = 0;
    int statsIntervalSeconds = 10;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT tracklet inspector " << tmcmt::kVersion << " (protocol v" << tmcmt::kProtocolVersion
              << ")\n\n"
              << "Usage: " << program << " <tracklets.jsonl|tracklets.bin> [options]\n"
              << "       " << program << " --source zmq --endpoint tcp://host:5556 [options]\n\n"
              << "Options:\n"
              << "  --source <kind>  file|zmq (default: file)\n"
              << "  --format <fmt>   auto|jsonl|protobuf (default: auto)\n"
              << "  --limit <n>      Read at most n tracklets (0 = no limit)\n"
              << "  --json           Print every tracklet as protobuf JSON\n"
              << "  --pretty         Pretty-print JSON output\n"
              << "  --endpoint <ep>  ZeroMQ SUB endpoint (default: tcp://127.0.0.1:5556)\n"
              << "  --prefix <p>     ZeroMQ subscription prefix (default: tmcmt.v1.)\n"
              << "  --timeout-ms <n> Per-message timeout (default: 1000)\n"
              << "  --duration <s>   Stop after this many seconds (0 = until Ctrl+C)\n"
              << "  --version        Print version and exit\n"
              << "  -h, --help       Print this help and exit\n";
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
        } else if (arg == "--json") {
            options.printJson = true;
        } else if (arg == "--pretty") {
            options.pretty = true;
        } else if (arg == "--source") {
            auto value = nextValue("--source");
            if (!value) {
                return std::nullopt;
            }
            if (tmcmt::detail::EqualsIgnoreCase(*value, "file")) {
                options.source = "file";
            } else if (tmcmt::detail::EqualsIgnoreCase(*value, "zmq")) {
                options.source = "zmq";
            } else {
                error = "unknown source '" + *value + "', expected one of: file|zmq";
                return std::nullopt;
            }
        } else if (arg == "--endpoint") {
            auto value = nextValue("--endpoint");
            if (!value) {
                return std::nullopt;
            }
            options.endpoint = std::move(*value);
        } else if (arg == "--prefix") {
            auto value = nextValue("--prefix");
            if (!value) {
                return std::nullopt;
            }
            options.subscribePrefix = std::move(*value);
        } else if (arg == "--timeout-ms") {
            auto value = nextValue("--timeout-ms");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.timeoutMs = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --timeout-ms: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--duration") {
            auto value = nextValue("--duration");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.durationSeconds = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --duration: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--stats-interval") {
            auto value = nextValue("--stats-interval");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.statsIntervalSeconds = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --stats-interval: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--format") {
            auto value = nextValue("--format");
            if (!value) {
                return std::nullopt;
            }
            if (tmcmt::detail::EqualsIgnoreCase(*value, "auto")) {
                options.format = tmcmt::TrackletFileFormat::Auto;
            } else if (tmcmt::detail::EqualsIgnoreCase(*value, "jsonl") ||
                       tmcmt::detail::EqualsIgnoreCase(*value, "json")) {
                options.format = tmcmt::TrackletFileFormat::Jsonl;
            } else if (tmcmt::detail::EqualsIgnoreCase(*value, "protobuf") ||
                       tmcmt::detail::EqualsIgnoreCase(*value, "pb") ||
                       tmcmt::detail::EqualsIgnoreCase(*value, "bin")) {
                options.format = tmcmt::TrackletFileFormat::Protobuf;
            } else {
                error = "unknown format '" + *value + "', expected one of: auto|jsonl|protobuf";
                return std::nullopt;
            }
        } else if (arg == "--limit") {
            auto value = nextValue("--limit");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.limit = static_cast<std::size_t>(std::stoull(*value));
            } catch (const std::exception&) {
                error = "invalid value for --limit: " + *value;
                return std::nullopt;
            }
        } else if (!arg.empty() && arg.front() == '-') {
            error = "unknown argument '" + arg + "'";
            return std::nullopt;
        } else if (options.path.empty()) {
            options.path = arg;
        } else {
            error = "unexpected argument '" + arg + "'";
            return std::nullopt;
        }
    }

    if (!options.showHelp && !options.showVersion && options.source == "file" && options.path.empty()) {
        error = "missing input file";
        return std::nullopt;
    }
    return options;
}

struct Stats {
    std::size_t tracklets = 0;
    std::size_t observations = 0;
    std::size_t finalChunks = 0;
    std::size_t totalBytes = 0;
    std::set<std::string> sessions;
    std::set<tmcmt::CameraId> cameras;
    std::map<std::uint16_t, std::size_t> embeddingDims;
    std::map<tmcmt::ObjectType, std::size_t> types;
    std::optional<tmcmt::Timestamp> first;
    std::optional<tmcmt::Timestamp> last;
    double embeddingNormMin = 0.0;
    double embeddingNormMax = 0.0;
    bool haveNorm = false;
};

void Accumulate(Stats& stats, const tmcmt::Tracklet& tracklet, std::size_t bytes) {
    ++stats.tracklets;
    stats.observations += tracklet.observations.size();
    if (tracklet.is_end) {
        ++stats.finalChunks;
    }
    stats.totalBytes += bytes;
    stats.sessions.insert(tracklet.session_id);
    stats.cameras.insert(tracklet.camera);
    ++stats.embeddingDims[tracklet.embedding.dim];
    ++stats.types[tracklet.type];

    if (!stats.first || tracklet.begin < *stats.first) {
        stats.first = tracklet.begin;
    }
    if (!stats.last || tracklet.end > *stats.last) {
        stats.last = tracklet.end;
    }

    const double norm = tracklet.embedding.Norm();
    if (!stats.haveNorm) {
        stats.embeddingNormMin = norm;
        stats.embeddingNormMax = norm;
        stats.haveNorm = true;
    } else {
        stats.embeddingNormMin = std::min(stats.embeddingNormMin, norm);
        stats.embeddingNormMax = std::max(stats.embeddingNormMax, norm);
    }
}

void PrintSummary(const Stats& stats, const std::string& sourceLine) {
    std::cout << sourceLine << '\n'
              << "tracklets: " << stats.tracklets << '\n'
              << "observations: " << stats.observations << '\n'
              << "final_chunks: " << stats.finalChunks << '\n'
              << "payload_bytes: " << stats.totalBytes << '\n'
              << "sessions: " << stats.sessions.size();
    if (!stats.sessions.empty()) {
        std::cout << " [";
        bool firstItem = true;
        for (const std::string& session : stats.sessions) {
            std::cout << (firstItem ? "" : ", ") << session;
            firstItem = false;
        }
        std::cout << ']';
    }
    std::cout << "\ncameras: " << stats.cameras.size();
    if (!stats.cameras.empty()) {
        std::cout << " [";
        bool firstItem = true;
        for (const tmcmt::CameraId& camera : stats.cameras) {
            std::cout << (firstItem ? "" : ", ") << camera;
            firstItem = false;
        }
        std::cout << ']';
    }
    std::cout << '\n';

    if (stats.first && stats.last) {
        std::cout << "time_range_ms: [" << stats.first->count() << ", " << stats.last->count() << "]\n";
    }
    if (!stats.embeddingDims.empty()) {
        std::cout << "embedding_dims:";
        for (const auto& [dim, count] : stats.embeddingDims) {
            std::cout << ' ' << dim << 'x' << count;
        }
        std::cout << '\n';
    }
    if (stats.haveNorm) {
        std::cout << "embedding_norm: [" << stats.embeddingNormMin << ", " << stats.embeddingNormMax << "]\n";
    }
    if (!stats.types.empty()) {
        std::cout << "object_types:";
        for (const auto& [type, count] : stats.types) {
            std::cout << ' ' << tmcmt::ToString(type) << '=' << count;
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string parseError;
    std::optional<Options> options = ParseArgs(argc, argv, parseError);
    if (!options) {
        std::cerr << "error: " << parseError << "\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_tracklet_inspect");
        return kExitUsageError;
    }
    if (options->showHelp) {
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_tracklet_inspect");
        return kExitOk;
    }
    if (options->showVersion) {
        std::cout << "TrafficMCMT " << tmcmt::kVersion << " (protocol v" << tmcmt::kProtocolVersion << ")\n";
        return kExitOk;
    }

    Stats stats;
    tmcmt::Tracklet tracklet;

#ifdef TMCMT_WITH_ZMQ
    if (options->source == "zmq") {
        tmcmt::net::ZmqSourceOptions zmqOptions;
        zmqOptions.endpoint = options->endpoint;
        zmqOptions.bind = false;
        zmqOptions.subscribe_prefix = options->subscribePrefix;
        zmqOptions.timeout_ms = options->timeoutMs;
        zmqOptions.normalize = true;
        zmqOptions.strict = false;

        tmcmt::net::ZmqTrackletSource source;
        const auto startStatus = source.Start(zmqOptions);
        if (!startStatus.ok()) {
            std::cerr << "error: " << startStatus.ToString() << '\n';
            return kExitDataError;
        }

        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);

        const auto start = std::chrono::steady_clock::now();
        std::size_t timeouts = 0;
        for (;;) {
            if (g_stop.load()) {
                break;
            }

            auto next = source.Next(tracklet);
            if (!next) {
                if (next.status().code() == tmcmt::ErrorCode::Timeout) {
                    ++timeouts;
                    if (options->durationSeconds > 0 &&
                        std::chrono::steady_clock::now() - start >=
                            std::chrono::seconds(options->durationSeconds)) {
                        break;
                    }
                    continue;
                }
                std::cerr << "error: " << next.status().ToString() << '\n';
                source.Stop();
                return kExitDataError;
            }
            if (!next.value()) {
                break;
            }

            auto encoded = tmcmt::TrackletCodec::EncodeBinary(tracklet);
            const std::size_t lineBytes = encoded ? encoded.value().size() : 0;
            Accumulate(stats, tracklet, lineBytes);

            if (options->printJson) {
                auto json = tmcmt::TrackletCodec::EncodeJson(tracklet, options->pretty);
                if (!json) {
                    std::cerr << "error: " << json.status().ToString() << '\n';
                    source.Stop();
                    return kExitDataError;
                }
                std::cout << json.value() << '\n';
            }

            if (options->limit != 0 && stats.tracklets >= options->limit) {
                break;
            }
            if (options->durationSeconds > 0 &&
                std::chrono::steady_clock::now() - start >=
                    std::chrono::seconds(options->durationSeconds)) {
                break;
            }
        }

        const std::string endpoint = source.endpoint();
        const tmcmt::net::ZmqSourceStats sourceStats = source.stats();
        source.Stop();

        PrintSummary(stats, "source: zmq(" + endpoint + ")");
        std::cout << "zmq_received: " << sourceStats.received << '\n'
                  << "zmq_decoded: " << sourceStats.decoded << '\n'
                  << "zmq_rejected: " << sourceStats.rejected << '\n'
                  << "zmq_malformed: " << sourceStats.malformed << '\n'
                  << "zmq_duplicates: " << sourceStats.duplicates << '\n'
                  << "zmq_missing_total: " << sourceStats.missing_total << '\n'
                  << "zmq_out_of_order: " << sourceStats.out_of_order << '\n'
                  << "zmq_timeouts: " << timeouts << '\n'
                  << "zmq_bytes_received: " << sourceStats.bytes_received << '\n';
        return kExitOk;
    }
#else
    if (options->source == "zmq") {
        std::cerr << "error: this build has no ZeroMQ transport "
                     "(configure with -DTRAFFICMCMT_WITH_ZMQ=ON)\n";
        return kExitDataError;
    }
#endif

    tmcmt::FileTrackletSource::Options sourceOptions;
    sourceOptions.format = options->format;
    sourceOptions.speed = 0.0; // Never pace an offline inspection.

    auto source = tmcmt::FileTrackletSource::Open(options->path, sourceOptions);
    if (!source) {
        std::cerr << "error: " << source.status().ToString() << '\n';
        return kExitDataError;
    }

    for (;;) {
        auto next = source.value()->Next(tracklet);
        if (!next) {
            std::cerr << "error: " << next.status().ToString() << '\n';
            return kExitDataError;
        }
        if (!next.value()) {
            break;
        }

        auto encoded = tmcmt::TrackletCodec::EncodeBinary(tracklet);
        const std::size_t lineBytes = encoded ? encoded.value().size() : 0;
        Accumulate(stats, tracklet, lineBytes);

        if (options->printJson) {
            auto json = tmcmt::TrackletCodec::EncodeJson(tracklet, options->pretty);
            if (!json) {
                std::cerr << "error: " << json.status().ToString() << '\n';
                return kExitDataError;
            }
            std::cout << json.value() << '\n';
        }

        if (options->limit != 0 && stats.tracklets >= options->limit) {
            break;
        }
    }

    PrintSummary(stats, "file: " + source.value()->Describe());
    return kExitOk;
}
