#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/map/MapConfig.h"
#include "trafficmcmt/map/MapFeed.h"
#include "trafficmcmt/map/MapTypes.h"
#include "trafficmcmt/map/TrackletFiles.h"
#include "trafficmcmt/map/TrackletTimeline.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitRuntimeError = 3;

struct Options {
    std::vector<std::string> inputs;
    std::string config = "configs/map_feed.yaml";
    std::string output = "build/map/frames.jsonl";
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT map export " << tmcmt::kVersion << "\n\n"
              << "Converts Edge tracklets into viewer protocol v1 frames (JSONL).\n\n"
              << "Usage: " << program << " --input <tracklets.jsonl> [--input ...] [options]\n\n"
              << "Options:\n"
              << "  --input <path>    Tracklet file (JSONL or protobuf); repeatable\n"
              << "  --config <yaml>   Map feed config (default: configs/map_feed.yaml)\n"
              << "  --out <path>      Output frames file (default: build/map/frames.jsonl)\n"
              << "  --version         Print version and exit\n"
              << "  -h, --help        Print this help and exit\n";
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
        } else if (arg == "--out") {
            auto value = nextValue("--out");
            if (!value) {
                return std::nullopt;
            }
            options.output = std::move(*value);
        } else {
            error = "unknown option '" + arg + "'";
            return std::nullopt;
        }
    }
    return options;
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

    auto origin = tmcmt::map::ResolveMapOrigin(tracklets.value(), config.value());
    if (!origin) {
        std::cerr << "error: " << origin.status().ToString() << '\n';
        return kExitRuntimeError;
    }
    tmcmt::map::MapFeedConfig feedConfig = std::move(config).value();
    feedConfig.has_origin = true;
    feedConfig.origin_latitude = origin.value().origin.latitude;
    feedConfig.origin_longitude = origin.value().origin.longitude;

    const auto outputPath = std::filesystem::path(options->output);
    if (outputPath.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) {
            std::cerr << "error: cannot create output directory: " << error.message() << '\n';
            return kExitRuntimeError;
        }
    }
    std::ofstream output(options->output);
    if (!output.is_open()) {
        std::cerr << "error: cannot open output file: " << options->output << '\n';
        return kExitRuntimeError;
    }

    tmcmt::map::TrackletTimeline timeline;
    std::size_t observationCount = 0;
    for (const tmcmt::Tracklet& tracklet : tracklets.value()) {
        observationCount += tracklet.observations.size();
    }
    for (tmcmt::Tracklet& tracklet : tracklets.value()) {
        timeline.Add(std::move(tracklet));
    }

    tmcmt::map::MapFeed feed(feedConfig);
    const tmcmt::Timestamp tick(1000 / feedConfig.tick_hz);
    const tmcmt::Timestamp begin = timeline.Begin().value_or(tmcmt::Timestamp(0));

    std::size_t frameCount = 0;
    const auto writeFrame = [&](const tmcmt::map::MapFrame& frame) {
        output << tmcmt::map::ToJson(frame) << '\n';
        ++frameCount;
    };

    writeFrame(feed.CameraZones(begin));

    tmcmt::Timestamp nextTick = begin;
    tmcmt::map::ObservationEvent event;
    tmcmt::Timestamp nextTimestamp;
    while (timeline.PeekTimestamp(nextTimestamp)) {
        while (nextTimestamp > nextTick) {
            for (const auto& frame : feed.Tick(nextTick)) {
                writeFrame(frame);
            }
            nextTick += tick;
        }
        timeline.Next(event);
        feed.Observe(*event.tracklet, *event.observation);
    }

    const tmcmt::Timestamp end = timeline.End().value_or(begin);
    const tmcmt::Timestamp ttl(static_cast<std::int64_t>(feedConfig.track_ttl_s * 1000.0));
    const tmcmt::Timestamp stop = end + ttl + tick;
    while (nextTick <= stop) {
        for (const auto& frame : feed.Tick(nextTick)) {
            writeFrame(frame);
        }
        nextTick += tick;
    }

    std::cout << "map-export: tracklets=" << timeline.TrackletCount() << " observations=" << observationCount
              << " frames=" << frameCount << " active_tracks=" << feed.ActiveTracks()
              << " unknown_camera_observations=" << feed.UnknownCameraObservations() << '\n';
    std::cout << "map-export: origin=" << feedConfig.origin_latitude << ',' << feedConfig.origin_longitude
              << (origin.value().from_stream ? " (stream)" : " (config)") << '\n';
    std::cout << "map-export: wrote " << options->output << '\n';
    return kExitOk;
}
