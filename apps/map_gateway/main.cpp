#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/map/MapConfig.h"
#include "trafficmcmt/map/MapFeed.h"
#include "trafficmcmt/map/MapTypes.h"
#include "trafficmcmt/map/TrackletFiles.h"
#include "trafficmcmt/map/TrackletTimeline.h"
#include "trafficmcmt/net/WebSocketServer.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitRuntimeError = 3;

std::atomic<bool> g_stop{false};

void HandleSignal(int) {
    g_stop = true;
}

struct Options {
    std::vector<std::string> inputs;
    std::string config = "configs/map_feed.yaml";
    std::string host = "0.0.0.0";
    std::uint16_t port = 8080;
    std::string path = "/ws";
    /// Track seconds per wall second. 0 means "replay as fast as possible".
    double speed = 1.0;
    bool loop = false;
    bool exitOnEnd = false;
    /// Optional wall-clock limit, useful for smoke tests.
    double durationS = 0.0;
    /// Optional JSONL dump of every broadcast frame (debugging / replay).
    std::string framesOut;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT map gateway " << tmcmt::kVersion << "\n\n"
              << "Replays Edge tracklets over WebSocket using viewer protocol v1.\n\n"
              << "Usage: " << program << " --input <tracklets.jsonl> [--input ...] [options]\n\n"
              << "Options:\n"
              << "  --input <path>     Tracklet file (JSONL or protobuf); repeatable\n"
              << "  --config <yaml>    Map feed config (default: configs/map_feed.yaml)\n"
              << "  --host <addr>      Bind address (default: 0.0.0.0)\n"
              << "  --port <n>         TCP port (default: 8080, 0 = ephemeral)\n"
              << "  --path <path>      WebSocket endpoint (default: /ws)\n"
              << "  --speed <factor>   Replay speed, 0 = no pacing (default: 1.0)\n"
              << "  --loop             Restart the replay when the input ends\n"
              << "  --exit-on-end      Exit once all tracks have expired\n"
              << "  --duration <s>     Stop after N wall-clock seconds (0 = unlimited)\n"
              << "  --frames-out <p>   Append every broadcast frame to a JSONL file\n"
              << "  --version          Print version and exit\n"
              << "  -h, --help         Print this help and exit\n";
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
        } else if (arg == "--input") {
            auto value = nextValue("--input");
            if (!value) {
                return std::nullopt;
            }
            options.inputs.push_back(std::move(*value));
        } else if (arg == "--config") {
            auto value = nextValue("--config");
            if (!value) {
                return std::nullopt;
            }
            options.config = std::move(*value);
        } else if (arg == "--host") {
            auto value = nextValue("--host");
            if (!value) {
                return std::nullopt;
            }
            options.host = std::move(*value);
        } else if (arg == "--path") {
            auto value = nextValue("--path");
            if (!value) {
                return std::nullopt;
            }
            options.path = std::move(*value);
        } else if (arg == "--frames-out") {
            auto value = nextValue("--frames-out");
            if (!value) {
                return std::nullopt;
            }
            options.framesOut = std::move(*value);
        } else if (arg == "--port") {
            auto value = nextValue("--port");
            if (!value) {
                return std::nullopt;
            }
            const int port = std::stoi(*value);
            if (port < 0 || port > 65535) {
                error = "invalid --port '" + *value + "'";
                return std::nullopt;
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (arg == "--speed") {
            auto value = nextValue("--speed");
            if (!value) {
                return std::nullopt;
            }
            options.speed = std::stod(*value);
            if (options.speed < 0.0) {
                error = "invalid --speed '" + *value + "'";
                return std::nullopt;
            }
        } else if (arg == "--duration") {
            auto value = nextValue("--duration");
            if (!value) {
                return std::nullopt;
            }
            options.durationS = std::stod(*value);
            if (options.durationS < 0.0) {
                error = "invalid --duration '" + *value + "'";
                return std::nullopt;
            }
        } else if (arg == "--loop") {
            options.loop = true;
        } else if (arg == "--exit-on-end") {
            options.exitOnEnd = true;
        } else {
            error = "unknown option '" + arg + "'";
            return std::nullopt;
        }
    }

    if (options.loop && options.exitOnEnd) {
        error = "--loop and --exit-on-end are mutually exclusive";
        return std::nullopt;
    }
    return options;
}

using WallClock = std::chrono::steady_clock;

/// Serialises broadcast frames to an optional JSONL file.
class FrameDumper {
public:
    explicit FrameDumper(const std::string& path) {
        if (path.empty()) {
            return;
        }
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
        }
        m_stream.open(path, std::ios::out | std::ios::trunc);
    }

    void Write(const std::string& frame) {
        if (!m_stream.is_open()) {
            return;
        }
        m_stream << frame << '\n';
    }

    void Flush() {
        if (m_stream.is_open()) {
            m_stream.flush();
        }
    }

private:
    std::ofstream m_stream;
};

} // namespace

int main(int argc, char** argv) {
    std::string parseError;
    std::optional<Options> options;
    try {
        options = ParseArgs(argc, argv, parseError);
    } catch (const std::exception& error) {
        parseError = error.what();
        options = std::nullopt;
    }
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
    if (options->inputs.empty()) {
        std::cerr << "error: at least one --input is required\n\n";
        PrintUsage(argv[0]);
        return kExitUsageError;
    }

    auto config = tmcmt::map::LoadMapFeedConfig(options->config);
    if (!config) {
        std::cerr << "error: " << config.status().ToString() << '\n';
        return kExitRuntimeError;
    }

    auto tracklets = tmcmt::map::LoadTrackletFiles(options->inputs);
    if (!tracklets) {
        std::cerr << "error: " << tracklets.status().ToString() << '\n';
        return kExitRuntimeError;
    }
    if (tracklets.value().empty()) {
        std::cerr << "error: input files contain no tracklets\n";
        return kExitRuntimeError;
    }

    auto origin = tmcmt::map::ResolveMapOrigin(tracklets.value(), config.value());
    if (!origin) {
        std::cerr << "error: " << origin.status().ToString() << '\n';
        return kExitRuntimeError;
    }

    tmcmt::map::MapFeedConfig feedConfig = std::move(config).value();
    feedConfig.has_origin = true;
    feedConfig.origin_latitude = origin.value().origin.latitude;
    feedConfig.origin_longitude = origin.value().origin.longitude;

    // The timeline owns the tracklets, so build it once and restart it from a
    // saved copy on every loop iteration.
    std::vector<tmcmt::Tracklet> owned = std::move(tracklets).value();

    tmcmt::map::TrackletTimeline timeline;
    for (const tmcmt::Tracklet& tracklet : owned) {
        timeline.Add(tracklet);
    }
    const tmcmt::Timestamp trackBegin = timeline.Begin().value_or(tmcmt::Timestamp(0));
    const tmcmt::Timestamp trackEnd = timeline.End().value_or(trackBegin);
    const tmcmt::Timestamp tickInterval(std::max<std::int64_t>(1, 1000 / feedConfig.tick_hz));
    const tmcmt::Timestamp ttl(static_cast<std::int64_t>(feedConfig.track_ttl_s * 1000.0));

    std::mutex feedMutex;
    auto feed = std::make_unique<tmcmt::map::MapFeed>(feedConfig);
    // Track time of the last emitted frame; also used for snapshots.
    tmcmt::Timestamp feedNow = trackBegin;

    FrameDumper dumper(options->framesOut);

    tmcmt::net::WebSocketServerOptions serverOptions;
    serverOptions.host = options->host;
    serverOptions.port = options->port;
    serverOptions.path = options->path;

    tmcmt::net::WebSocketServer server(serverOptions);

    server.SetHealthProvider([&feedMutex, &feed]() {
        std::lock_guard<std::mutex> lock(feedMutex);
        return std::string("{\"status\":\"ok\",\"protocol\":1,\"active_tracks\":") +
               std::to_string(feed->ActiveTracks()) + "}";
    });

    server.SetClientConnectedHandler([&](std::uint64_t clientId) {
        // New clients need the static layer plus the current state before any
        // deltas arrive. Both frames are queued on the client's outbox, so the
        // order is preserved.
        std::vector<std::string> frames;
        {
            std::lock_guard<std::mutex> lock(feedMutex);
            frames.push_back(tmcmt::map::ToJson(feed->CameraZones(feedNow)));
            frames.push_back(tmcmt::map::ToJson(feed->Snapshot(feedNow)));
            if (const std::optional<tmcmt::map::MapFrame> emission = feed->EmissionSnapshot(feedNow)) {
                frames.push_back(tmcmt::map::ToJson(*emission));
            }
        }
        for (const std::string& frame : frames) {
            server.SendTo(clientId, frame);
        }
    });

    const auto status = server.Start();
    if (!status) {
        std::cerr << "error: " << status.ToString() << '\n';
        return kExitRuntimeError;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::cout << "map-gateway: ws://" << options->host << ':' << server.Port() << options->path << '\n'
              << "map-gateway: tracklets=" << owned.size() << " origin=" << feedConfig.origin_latitude << ','
              << feedConfig.origin_longitude << (origin.value().from_stream ? " (stream)" : " (config)")
              << " speed=" << options->speed << '\n';

    const auto wallStart = WallClock::now();
    const double speed = options->speed;
    bool finished = false;

    // `replayOffset` is the track timestamp that maps to `replayWallStart`. It
    // moves forward on every loop iteration.
    tmcmt::Timestamp replayOffset = trackBegin;
    auto replayWallStart = wallStart;

    auto restart = [&]() {
        std::lock_guard<std::mutex> lock(feedMutex);
        feed = std::make_unique<tmcmt::map::MapFeed>(feedConfig);
        feedNow = trackBegin;
        timeline.Clear();
        for (const tmcmt::Tracklet& tracklet : owned) {
            timeline.Add(tracklet);
        }
        replayOffset = trackBegin;
        replayWallStart = WallClock::now();
    };

    while (!g_stop) {
        const auto wallNow = WallClock::now();
        const double elapsedS = std::chrono::duration<double>(wallNow - wallStart).count();
        if (options->durationS > 0.0 && elapsedS >= options->durationS) {
            break;
        }

        // Target track time for this iteration.
        tmcmt::Timestamp target = trackEnd;
        if (speed > 0.0) {
            const double replayElapsedS =
                std::chrono::duration<double>(wallNow - replayWallStart).count();
            const auto advance = tmcmt::Timestamp(static_cast<std::int64_t>(replayElapsedS * speed * 1000.0));
            target = replayOffset + advance;
        }

        std::vector<std::string> out;
        bool atEnd = false;
        {
            std::lock_guard<std::mutex> lock(feedMutex);

            tmcmt::map::ObservationEvent event;
            tmcmt::Timestamp nextTimestamp;
            while (timeline.PeekTimestamp(nextTimestamp) && nextTimestamp <= target) {
                timeline.Next(event);
                feed->Observe(*event.tracklet, *event.observation);
            }

            // Once the input is drained, advance the clock past the TTL so the
            // final `tracks_remove` frames are emitted even without pacing.
            if (timeline.Empty()) {
                target = std::max(target, trackEnd + ttl + tickInterval);
            }

            while (feedNow + tickInterval <= target) {
                feedNow += tickInterval;
                for (const tmcmt::map::MapFrame& frame : feed->Tick(feedNow)) {
                    out.push_back(tmcmt::map::ToJson(frame));
                }
            }

            atEnd = timeline.Empty() && feedNow >= trackEnd + ttl;
        }

        for (const std::string& frame : out) {
            server.Broadcast(frame);
            dumper.Write(frame);
        }

        if (atEnd) {
            if (options->loop) {
                restart();
            } else if (options->exitOnEnd) {
                finished = true;
            }
        }

        if (finished) {
            break;
        }

        if (speed > 0.0) {
            // Keep the producer near real time without busy-waiting.
            const auto work = WallClock::now() - wallNow;
            const auto sleepFor = std::chrono::milliseconds(5) - work;
            if (sleepFor > std::chrono::milliseconds(0)) {
                std::this_thread::sleep_for(sleepFor);
            }
        }
    }

    server.Stop();
    dumper.Flush();

    std::lock_guard<std::mutex> lock(feedMutex);
    std::cout << "map-gateway: stopped, active_tracks=" << feed->ActiveTracks()
              << " unknown_camera_observations=" << feed->UnknownCameraObservations() << '\n';
    return kExitOk;
}
