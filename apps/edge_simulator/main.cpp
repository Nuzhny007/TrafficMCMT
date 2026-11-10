#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/net/ZmqTrackletPublisher.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitRuntimeError = 3;

std::atomic<bool> g_stop{false};

void HandleSignal(int) {
    g_stop.store(true);
}

struct Options {
    std::string endpoint = "tcp://127.0.0.1:5555";
    bool bind = false;
    int cameras = 1;
    double rate = 1.0; // tracklets per second per camera
    int durationSeconds = 0;
    int statsIntervalSeconds = 10;
    unsigned seed = 42;
    std::uint16_t embeddingDim = 32;
    std::size_t observations = 10;
    std::uint64_t chunksPerTrack = 5;
    std::string sessionId;
    std::string cameraPrefix = "cam";
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT synthetic Edge cameras " << tmcmt::kVersion << "\n\n"
              << "Publishes TrackletChunk messages over ZeroMQ PUB.\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  --endpoint <ep>        PUB endpoint (default: tcp://127.0.0.1:5555)\n"
              << "  --bind                 Bind instead of connect\n"
              << "  --cameras <n>          Number of virtual cameras (default: 1)\n"
              << "  --rate <hz>            Tracklets per second per camera (default: 1)\n"
              << "  --duration <seconds>   Stop after this many seconds (0 = until Ctrl+C)\n"
              << "  --seed <n>             Random seed (default: 42)\n"
              << "  --embedding-dim <n>    Embedding dimension (default: 32)\n"
              << "  --observations <n>     Observations per chunk (default: 10)\n"
              << "  --chunks-per-track <n> Chunks before TRACK_END (default: 5)\n"
              << "  --session-id <id>      Session id (default: generated)\n"
              << "  --camera-prefix <p>    Camera id prefix (default: cam)\n"
              << "  --stats-interval <s>   Print statistics every N seconds (default: 10)\n"
              << "  --version              Print version and exit\n"
              << "  -h, --help             Print this help and exit\n";
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
        const auto parseNumber = [&](const char* name, auto& target, const char* type) -> bool {
            auto value = nextValue(name);
            if (!value) {
                return false;
            }
            try {
                std::istringstream stream(*value);
                stream >> target;
                if (stream.fail()) {
                    throw std::invalid_argument("bad value");
                }
            } catch (const std::exception&) {
                error = std::string("invalid ") + name + " value '" + *value + "'";
                return false;
            }
            (void)type;
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        } else if (arg == "--version") {
            options.showVersion = true;
        } else if (arg == "--bind") {
            options.bind = true;
        } else if (arg == "--endpoint") {
            auto value = nextValue("--endpoint");
            if (!value) {
                return std::nullopt;
            }
            options.endpoint = std::move(*value);
        } else if (arg == "--cameras") {
            if (!parseNumber("--cameras", options.cameras, "int")) {
                return std::nullopt;
            }
        } else if (arg == "--rate") {
            if (!parseNumber("--rate", options.rate, "double")) {
                return std::nullopt;
            }
        } else if (arg == "--duration") {
            if (!parseNumber("--duration", options.durationSeconds, "int")) {
                return std::nullopt;
            }
        } else if (arg == "--seed") {
            if (!parseNumber("--seed", options.seed, "unsigned")) {
                return std::nullopt;
            }
        } else if (arg == "--embedding-dim") {
            if (!parseNumber("--embedding-dim", options.embeddingDim, "uint16")) {
                return std::nullopt;
            }
        } else if (arg == "--observations") {
            if (!parseNumber("--observations", options.observations, "size_t")) {
                return std::nullopt;
            }
        } else if (arg == "--chunks-per-track") {
            if (!parseNumber("--chunks-per-track", options.chunksPerTrack, "uint64")) {
                return std::nullopt;
            }
        } else if (arg == "--session-id") {
            auto value = nextValue("--session-id");
            if (!value) {
                return std::nullopt;
            }
            options.sessionId = std::move(*value);
        } else if (arg == "--camera-prefix") {
            auto value = nextValue("--camera-prefix");
            if (!value) {
                return std::nullopt;
            }
            options.cameraPrefix = std::move(*value);
        } else if (arg == "--stats-interval") {
            if (!parseNumber("--stats-interval", options.statsIntervalSeconds, "int")) {
                return std::nullopt;
            }
        } else {
            error = "unknown option '" + arg + "'";
            return std::nullopt;
        }
    }
    return options;
}

struct CameraState {
    std::string camera;
    std::uint64_t chunks = 0;
    std::uint64_t published = 0;
    std::uint64_t failures = 0;
    std::uint64_t localTrackId = 0;
};

std::string MakeSessionId() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::ostringstream stream;
    stream << "sim-" << ms;
    return stream.str();
}

std::string MakeCameraId(const std::string& prefix, int index, int total) {
    const int width = total >= 100 ? 3 : 2;
    std::ostringstream stream;
    stream << prefix << '-' << std::setw(width) << std::setfill('0') << index;
    return stream.str();
}

tmcmt::Tracklet MakeTracklet(const CameraState& camera, const Options& options, std::mt19937& rng,
                             tmcmt::Timestamp now) {
    tmcmt::Tracklet tracklet;
    tracklet.session_id = options.sessionId;
    tracklet.camera = camera.camera;
    tracklet.local_track_id = camera.localTrackId;
    tracklet.sequence = camera.chunks;
    tracklet.is_end = options.chunksPerTrack > 0 && (camera.chunks + 1) % options.chunksPerTrack == 0;
    tracklet.type = tmcmt::ObjectType::Car;
    tracklet.quality = 0.9f;

    const auto chunkDuration = std::chrono::milliseconds(
        static_cast<std::int64_t>(1000.0 / std::max(0.001, options.rate)));
    tracklet.begin = now - chunkDuration;
    tracklet.end = now;

    std::uniform_real_distribution<float> uniform(0.f, 1.f);
    const float baseX = uniform(rng) * 1000.f;
    const float baseY = uniform(rng) * 700.f;

    tracklet.observations.reserve(options.observations);
    for (std::size_t i = 0; i < options.observations; ++i) {
        tmcmt::Observation observation;
        const double fraction = options.observations > 1
                                    ? static_cast<double>(i) / static_cast<double>(options.observations - 1)
                                    : 1.0;
        observation.ts = tracklet.begin + std::chrono::milliseconds(static_cast<std::int64_t>(
                                                      chunkDuration.count() * fraction));
        observation.bbox.x = baseX + static_cast<float>(i) * 2.f;
        observation.bbox.y = baseY + static_cast<float>(i) * 0.5f;
        observation.bbox.w = 40.f + uniform(rng) * 20.f;
        observation.bbox.h = 80.f + uniform(rng) * 30.f;
        observation.bbox.score = 0.6f + uniform(rng) * 0.4f;
        observation.world.x = baseX * 0.05f;
        observation.world.y = baseY * 0.05f;
        observation.world.valid = true;
        tracklet.observations.push_back(observation);
    }

    tracklet.embedding.dim = options.embeddingDim;
    tracklet.embedding.model_id = 1;
    tracklet.embedding.values.resize(options.embeddingDim);
    std::normal_distribution<float> normal(0.f, 1.f);
    float norm = 0.f;
    for (float& value : tracklet.embedding.values) {
        value = normal(rng);
        norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm > 0.f) {
        for (float& value : tracklet.embedding.values) {
            value /= norm;
        }
    }
    return tracklet;
}

} // namespace

int main(int argc, char** argv) {
    std::string parseError;
    auto options = ParseArgs(argc, argv, parseError);
    if (!options) {
        std::cerr << "error: " << parseError << "\n\n";
        PrintUsage(argv[0]);
        return kExitUsageError;
    }
    if (options->showHelp) {
        PrintUsage(argv[0]);
        return kExitOk;
    }
    if (options->showVersion) {
        std::cout << tmcmt::kVersion << '\n';
        return kExitOk;
    }
    if (options->cameras <= 0) {
        std::cerr << "error: --cameras must be positive\n";
        return kExitUsageError;
    }
    if (options->rate <= 0.0) {
        std::cerr << "error: --rate must be positive\n";
        return kExitUsageError;
    }
    if (options->embeddingDim == 0 || options->observations == 0) {
        std::cerr << "error: --embedding-dim and --observations must be positive\n";
        return kExitUsageError;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (options->sessionId.empty()) {
        options->sessionId = MakeSessionId();
    }
    if (options->chunksPerTrack == 0) {
        options->chunksPerTrack = 1;
    }

    tmcmt::net::ZmqPublisherOptions publisherOptions;
    publisherOptions.endpoint = options->endpoint;
    publisherOptions.bind = options->bind;
    publisherOptions.linger_ms = 0;

    tmcmt::net::ZmqTrackletPublisher publisher;
    const auto status = publisher.Start(publisherOptions);
    if (!status.ok()) {
        std::cerr << "error: " << status.ToString() << '\n';
        return kExitRuntimeError;
    }

    std::vector<CameraState> cameras;
    cameras.reserve(static_cast<std::size_t>(options->cameras));
    for (int i = 0; i < options->cameras; ++i) {
        CameraState camera;
        camera.camera = MakeCameraId(options->cameraPrefix, i, options->cameras);
        cameras.push_back(std::move(camera));
    }

    std::mt19937 rng(options->seed);
    const double eventsPerSecond = options->rate * static_cast<double>(options->cameras);

    std::cout << "edge-simulator: session=" << options->sessionId << " cameras=" << options->cameras
              << " rate=" << options->rate << "/s/camera endpoint=" << publisher.endpoint() << '\n'
              << std::flush;

    const auto start = std::chrono::steady_clock::now();
    const auto statsInterval = std::chrono::seconds(options->statsIntervalSeconds > 0
                                                        ? options->statsIntervalSeconds
                                                        : 10);
    auto nextStats = start + statsInterval;
    std::uint64_t scheduled = 0;
    std::uint64_t failures = 0;

    while (!g_stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsedSeconds = std::chrono::duration<double>(now - start).count();
        const auto due = static_cast<std::uint64_t>(elapsedSeconds * eventsPerSecond);

        if (due <= scheduled) {
            // Sleep until the next event. If the OS timer is coarse, the next
            // iteration publishes a small catch-up burst.
            const double nextAt = static_cast<double>(scheduled + 1) / eventsPerSecond;
            const auto nextTime = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                              std::chrono::duration<double>(nextAt));
            if (nextTime > now) {
                std::this_thread::sleep_until(nextTime);
            }
            continue;
        }

        // Publish everything that is due. The cap protects against a huge
        // burst after the process was suspended for a long time.
        const std::uint64_t burst = std::min<std::uint64_t>(due - scheduled, 20'000);
        for (std::uint64_t i = 0; i < burst; ++i) {
            CameraState& camera = cameras[static_cast<std::size_t>((scheduled + i) % cameras.size())];
            const tmcmt::Timestamp timestamp(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            const tmcmt::Tracklet tracklet = MakeTracklet(camera, *options, rng, timestamp);
            const auto publishStatus = publisher.Publish(tracklet);
            if (publishStatus.ok()) {
                ++camera.published;
            } else {
                ++camera.failures;
                ++failures;
            }
            ++camera.chunks;
            if (camera.chunks % options->chunksPerTrack == 0) {
                ++camera.localTrackId;
            }
        }
        scheduled += burst;

        if (now >= nextStats) {
            std::cout << "edge-simulator: published=" << scheduled << " failures=" << failures
                      << " elapsed=" << static_cast<std::int64_t>(elapsedSeconds) << "s\n"
                      << std::flush;
            nextStats = now + statsInterval;
        }
        if (options->durationSeconds > 0 && elapsedSeconds >= options->durationSeconds) {
            break;
        }
    }

    const auto& stats = publisher.stats();
    publisher.Stop();
    std::cout << "edge-simulator: stopped after "
              << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start)
                     .count()
              << " s, published=" << stats.published << " failures=" << stats.send_failures
              << " bytes=" << stats.bytes_sent << '\n';
    return failures == 0 ? kExitOk : kExitRuntimeError;
}
