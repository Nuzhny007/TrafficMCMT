#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/background_segm.hpp>
#include <opencv2/videoio.hpp>

#include "mtracking/Ctracker.h"
#include "mtracking/defines.h"
#include "mtracking/trajectory.h"

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/edge/ColorHistogramReIdExtractor.h"
#include "trafficmcmt/edge/EdgeTrackExporter.h"
#include "trafficmcmt/io/ITrackletSink.h"
#include "trafficmcmt/io/TrackletSinks.h"
#include "trafficmcmt/mtt/MttTrackAdapter.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitRuntimeError = 3;

struct Options {
    std::string videoPath;
    std::string outputPath = "data/tracklets.jsonl";
    std::string cameraId = "cam-01";
    std::string sessionId;
    tmcmt::TrackletFileFormat format = tmcmt::TrackletFileFormat::Jsonl;
    bool formatExplicit = false;
    bool quantize = false;
    bool showHelp = false;
    bool showVersion = false;

    int maxFrames = 0;
    double speed = 0.0;
    double fpsOverride = 0.0;
    std::int64_t chunkIntervalMs = 1000;
    int history = 500;
    double varThreshold = 16.0;
    int minArea = 0;
    float minConfidence = 0.f;
    int minTraceLength = 2;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT MTT exporter " << tmcmt::kVersion << " (protocol v" << tmcmt::kProtocolVersion
              << ")\n\n"
              << "Usage: " << program << " --video <file> [options]\n\n"
              << "Options:\n"
              << "  --video <path>       Input video file (required)\n"
              << "  --out <path>         Output tracklet file (default: data/tracklets.jsonl)\n"
              << "  --format <fmt>       jsonl|protobuf (default: derived from --out extension)\n"
              << "  --camera <id>        Camera id written into every tracklet (default: cam-01)\n"
              << "  --session <id>       Session id (default: edge-<unix-ms>)\n"
              << "  --quantize           Store embeddings as int8 in the protobuf output\n"
              << "  --max-frames <n>     Stop after n frames (0 = whole video)\n"
              << "  --speed <x>          Playback speed, 0 = as fast as possible (default: 0)\n"
              << "  --fps <value>        Override the video frame rate\n"
              << "  --chunk-ms <n>       Tracklet chunk interval in milliseconds (default: 1000)\n"
              << "  --history <n>        MOG2 history length (default: 500)\n"
              << "  --var-threshold <x>  MOG2 variance threshold (default: 16)\n"
              << "  --min-area <px>      Minimum foreground contour area (default: automatic)\n"
              << "  --min-confidence <x> Minimum detector confidence, <0 disables (default: 0)\n"
              << "  --min-trace <n>      Minimum MTT trace length (default: 2)\n"
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
        } else if (arg == "--quantize") {
            options.quantize = true;
        } else if (arg == "--video") {
            auto value = nextValue("--video");
            if (!value) {
                return std::nullopt;
            }
            options.videoPath = std::move(*value);
        } else if (arg == "--out") {
            auto value = nextValue("--out");
            if (!value) {
                return std::nullopt;
            }
            options.outputPath = std::move(*value);
        } else if (arg == "--camera") {
            auto value = nextValue("--camera");
            if (!value) {
                return std::nullopt;
            }
            options.cameraId = std::move(*value);
        } else if (arg == "--session") {
            auto value = nextValue("--session");
            if (!value) {
                return std::nullopt;
            }
            options.sessionId = std::move(*value);
        } else if (arg == "--format") {
            auto value = nextValue("--format");
            if (!value) {
                return std::nullopt;
            }
            if (tmcmt::detail::EqualsIgnoreCase(*value, "jsonl") || tmcmt::detail::EqualsIgnoreCase(*value, "json")) {
                options.format = tmcmt::TrackletFileFormat::Jsonl;
            } else if (tmcmt::detail::EqualsIgnoreCase(*value, "protobuf") ||
                       tmcmt::detail::EqualsIgnoreCase(*value, "pb") ||
                       tmcmt::detail::EqualsIgnoreCase(*value, "bin")) {
                options.format = tmcmt::TrackletFileFormat::Protobuf;
            } else {
                error = "unknown format '" + *value + "', expected one of: jsonl|protobuf";
                return std::nullopt;
            }
            options.formatExplicit = true;
        } else if (arg == "--max-frames") {
            auto value = nextValue("--max-frames");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.maxFrames = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --max-frames: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--speed") {
            auto value = nextValue("--speed");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.speed = std::stod(*value);
            } catch (const std::exception&) {
                error = "invalid value for --speed: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--fps") {
            auto value = nextValue("--fps");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.fpsOverride = std::stod(*value);
            } catch (const std::exception&) {
                error = "invalid value for --fps: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--chunk-ms") {
            auto value = nextValue("--chunk-ms");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.chunkIntervalMs = std::stoll(*value);
            } catch (const std::exception&) {
                error = "invalid value for --chunk-ms: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--history") {
            auto value = nextValue("--history");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.history = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --history: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--var-threshold") {
            auto value = nextValue("--var-threshold");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.varThreshold = std::stod(*value);
            } catch (const std::exception&) {
                error = "invalid value for --var-threshold: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--min-area") {
            auto value = nextValue("--min-area");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.minArea = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --min-area: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--min-confidence") {
            auto value = nextValue("--min-confidence");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.minConfidence = std::stof(*value);
            } catch (const std::exception&) {
                error = "invalid value for --min-confidence: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--min-trace") {
            auto value = nextValue("--min-trace");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.minTraceLength = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --min-trace: " + *value;
                return std::nullopt;
            }
        } else {
            error = "unknown argument '" + arg + "'";
            return std::nullopt;
        }
    }

    if (!options.showHelp && !options.showVersion && options.videoPath.empty()) {
        error = "missing --video";
        return std::nullopt;
    }
    return options;
}

tmcmt::TrackletFileFormat DetectFormatFromPath(const std::string& path) {
    const std::filesystem::path extension = std::filesystem::path(path).extension();
    const std::string ext = extension.string();
    if (tmcmt::detail::EqualsIgnoreCase(ext, ".bin") || tmcmt::detail::EqualsIgnoreCase(ext, ".pb") ||
        tmcmt::detail::EqualsIgnoreCase(ext, ".protobuf") || tmcmt::detail::EqualsIgnoreCase(ext, ".tracklets")) {
        return tmcmt::TrackletFileFormat::Protobuf;
    }
    return tmcmt::TrackletFileFormat::Jsonl;
}

std::string MakeSessionId() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return "edge-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

int main(int argc, char** argv) {
    std::string parseError;
    std::optional<Options> options = ParseArgs(argc, argv, parseError);
    if (!options) {
        std::cerr << "error: " << parseError << "\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_mtt_export");
        return kExitUsageError;
    }
    if (options->showHelp) {
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_mtt_export");
        return kExitOk;
    }
    if (options->showVersion) {
        std::cout << "TrafficMCMT " << tmcmt::kVersion << " (protocol v" << tmcmt::kProtocolVersion << ")\n";
        return kExitOk;
    }

    cv::VideoCapture capture(options->videoPath);
    if (!capture.isOpened()) {
        std::cerr << "error: cannot open video: " << options->videoPath << '\n';
        return kExitRuntimeError;
    }

    double fps = options->fpsOverride;
    if (!(fps > 0.0)) {
        fps = capture.get(cv::CAP_PROP_FPS);
    }
    if (!(fps > 0.0) || !std::isfinite(fps)) {
        fps = 25.0;
    }

    if (!options->formatExplicit) {
        options->format = DetectFormatFromPath(options->outputPath);
    }
    if (options->sessionId.empty()) {
        options->sessionId = MakeSessionId();
    }

    std::unique_ptr<tmcmt::ITrackletSink> sink;
    if (options->format == tmcmt::TrackletFileFormat::Protobuf) {
        auto opened = tmcmt::BinaryTrackletSink::Open(options->outputPath, options->quantize);
        if (!opened) {
            std::cerr << "error: " << opened.status().ToString() << '\n';
            return kExitRuntimeError;
        }
        sink = std::move(opened).value();
    } else {
        auto opened = tmcmt::JsonlTrackletSink::Open(options->outputPath);
        if (!opened) {
            std::cerr << "error: " << opened.status().ToString() << '\n';
            return kExitRuntimeError;
        }
        sink = std::move(opened).value();
    }

    auto mog2 = cv::createBackgroundSubtractorMOG2(static_cast<int>(std::max(1, options->history)),
                                                   std::max(1.0, options->varThreshold), true);

    TrackerSettings settings;
    settings.SetDistance(tracking::DistRects);
    settings.m_kalmanType = tracking::KalmanLinear;
    settings.m_filterGoal = tracking::FilterCenter;
    settings.m_lostTrackType = tracking::TrackNone;
    settings.m_matchType = tracking::MatchHungrian;
    settings.m_dt = static_cast<float>(1.0 / fps);
    settings.m_distThres = 0.95f;
    settings.m_maximumAllowedSkippedFrames = static_cast<std::size_t>(std::lround(2.0 * fps));
    settings.m_maxTraceLength = static_cast<std::size_t>(std::lround(4.0 * fps));

    std::unique_ptr<BaseTracker> tracker = BaseTracker::CreateTracker(settings);
    if (!tracker) {
        std::cerr << "error: cannot create Multitarget-tracker instance\n";
        return kExitRuntimeError;
    }

    tmcmt::ColorHistogramOptions reidOptions;
    reidOptions.model_id = 1;
    tmcmt::ColorHistogramReIdExtractor extractor(reidOptions);

    tmcmt::MttAdapterOptions adapterOptions;
    adapterOptions.min_confidence = options->minConfidence;
    adapterOptions.min_trace_length = options->minTraceLength;
    tmcmt::MttTrackAdapter adapter(adapterOptions);

    tmcmt::EdgeExportConfig exportConfig;
    exportConfig.camera_id = options->cameraId;
    exportConfig.session_id = options->sessionId;
    exportConfig.chunk_interval_ms = options->chunkIntervalMs;
    exportConfig.model_id = reidOptions.model_id;
    tmcmt::EdgeTrackExporter exporter(exportConfig);

    const auto startEpoch =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());

    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::Mat frame;
    cv::Mat foreground;
    std::vector<std::vector<cv::Point>> contours;
    std::vector<tmcmt::Tracklet> chunks;

    std::size_t frames = 0;
    std::size_t chunksWritten = 0;
    std::size_t observationsWritten = 0;
    std::size_t reidFailures = 0;
    int minArea = options->minArea;

    const auto writeChunks = [&](std::vector<tmcmt::Tracklet>&& emitted) {
        for (const tmcmt::Tracklet& chunk : emitted) {
            const tmcmt::Status status = sink->Write(chunk);
            if (!status.ok()) {
                std::cerr << "error: " << status.ToString() << '\n';
                return false;
            }
            ++chunksWritten;
            observationsWritten += chunk.observations.size();
        }
        return true;
    };

    while (capture.read(frame) && !frame.empty()) {
        if (options->maxFrames > 0 && frames >= static_cast<std::size_t>(options->maxFrames)) {
            break;
        }
        if (frame.channels() == 1) {
            cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
        }
        if (minArea <= 0) {
            minArea = std::max(64, (frame.cols * frame.rows) / 20000);
        }

        mog2->apply(frame, foreground);
        cv::threshold(foreground, foreground, 200, 255, cv::THRESH_BINARY);
        cv::morphologyEx(foreground, foreground, cv::MORPH_OPEN, kernel);
        cv::morphologyEx(foreground, foreground, cv::MORPH_CLOSE, kernel);

        contours.clear();
        cv::findContours(foreground, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        regions_t regions;
        regions.reserve(contours.size());
        for (const auto& contour : contours) {
            const cv::Rect rect = cv::boundingRect(contour);
            if (rect.area() < minArea) {
                continue;
            }
            regions.emplace_back(rect);
        }

        tracker->UpdateMat(regions, frame, static_cast<float>(fps));

        const auto frameOffsetMs = static_cast<std::int64_t>(std::llround(static_cast<double>(frames) * 1000.0 / fps));
        const tmcmt::Timestamp now = startEpoch + std::chrono::milliseconds(frameOffsetMs);

        const std::vector<TrackingObject> tracks = tracker->GetTracksCopy();
        for (const TrackingObject& track : tracks) {
            if (!adapter.ShouldExport(track)) {
                continue;
            }

            auto observation = adapter.ToObservation(track, now);
            if (!observation) {
                continue;
            }

            const tmcmt::BBox bbox = observation.value().bbox;
            cv::Rect roi(cvRound(bbox.x), cvRound(bbox.y), cvRound(bbox.w), cvRound(bbox.h));
            roi &= cv::Rect(0, 0, frame.cols, frame.rows);
            if (roi.width < reidOptions.min_roi_size || roi.height < reidOptions.min_roi_size) {
                continue;
            }

            auto embedding = extractor.Extract(frame, roi);
            if (!embedding) {
                ++reidFailures;
                continue;
            }

            const tmcmt::ObjectType type = tmcmt::MttTrackAdapter::ToObjectType(track.m_type);
            chunks = exporter.AddObservation(track.m_ID.m_val, observation.value(), embedding.value(), type,
                                             track.m_confidence, now);
            if (!writeChunks(std::move(chunks))) {
                return kExitRuntimeError;
            }
        }

        chunks = exporter.Tick(now);
        if (!writeChunks(std::move(chunks))) {
            return kExitRuntimeError;
        }

        ++frames;

        if (options->speed > 0.0) {
            const auto frameDuration = std::chrono::duration<double>(1.0 / (fps * options->speed));
            std::this_thread::sleep_for(frameDuration);
        }
    }

    if (!writeChunks(exporter.Flush())) {
        return kExitRuntimeError;
    }
    const tmcmt::Status flushed = sink->Flush();
    if (!flushed.ok()) {
        std::cerr << "error: " << flushed.ToString() << '\n';
        return kExitRuntimeError;
    }

    std::cout << "video: " << options->videoPath << '\n'
              << "output: " << sink->Describe() << '\n'
              << "session: " << options->sessionId << '\n'
              << "camera: " << options->cameraId << '\n'
              << "frames: " << frames << '\n'
              << "fps: " << fps << '\n'
              << "tracklet_chunks: " << chunksWritten << '\n'
              << "observations: " << observationsWritten << '\n'
              << "reid_failures: " << reidFailures << '\n';
    return kExitOk;
}
