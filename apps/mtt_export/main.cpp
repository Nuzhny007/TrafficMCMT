#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/background_segm.hpp>
#include <opencv2/videoio.hpp>

#include "mdetection/BaseDetector.h"
#include "mtracking/BaseTracker.h"
#include "mtracking/defines.h"
#include "mtracking/object_types.h"
#include "mtracking/trajectory.h"

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/edge/ColorHistogramReIdExtractor.h"
#include "trafficmcmt/edge/EdgeTrackExporter.h"
#include "trafficmcmt/edge/GeoBinding.h"
#include "trafficmcmt/edge/IReIdExtractor.h"
#include "trafficmcmt/edge/TypeAwareReIdExtractor.h"
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
    int startFrame = 0;
    double speed = 0.0;
    double fpsOverride = 0.0;
    std::int64_t chunkIntervalMs = 1000;
    std::int64_t startEpochMs = 0;

    int history = 500;
    double varThreshold = 16.0;
    int minArea = 0;
    float minConfidence = 0.f;
    int minTraceLength = 2;

    std::string detectorSettings;
    std::string whiteList;
    float localCartesianDist = 10.f;
    std::string geoBind;
    std::string geoOrigin;

    std::string reidPersonModel;
    std::string reidVehicleModel;
    std::string reidInput;
    int reidBins = 8;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT MTT exporter " << tmcmt::kVersion << " (protocol v" << tmcmt::kProtocolVersion
              << ")\n\n"
              << "Usage: " << program << " --video <file> [options]\n\n"
              << "Options:\n"
              << "  --video <path>          Input video file (required)\n"
              << "  --out <path>            Output tracklet file (default: data/tracklets.jsonl)\n"
              << "  --format <fmt>          jsonl|protobuf (default: derived from --out extension)\n"
              << "  --camera <id>           Camera id written into every tracklet (default: cam-01)\n"
              << "  --session <id>          Session id (default: edge-<unix-ms>)\n"
              << "  --quantize              Store embeddings as int8 in the protobuf output\n"
              << "  --start-frame <n>       First video frame to process (default: 0)\n"
              << "  --max-frames <n>        Stop after n frames (0 = whole video)\n"
              << "  --speed <x>             Playback speed, 0 = as fast as possible (default: 0)\n"
              << "  --fps <value>           Override the video frame rate\n"
              << "  --chunk-ms <n>          Tracklet chunk interval in milliseconds (default: 1000)\n"
              << "  --start-epoch-ms <ms>   Fixed Unix time of the first frame (default: current time)\n\n"
              << "Detector:\n"
              << "  --detector-settings <ini>  Multitarget-tracker settings (opencv_dnn detector)\n"
              << "  --white-list <types>       Semicolon-separated object types to keep\n"
              << "  --min-confidence <x>       Minimum track confidence, <0 disables (default: 0)\n"
              << "  --min-trace <n>            Minimum MTT trace length (default: 2)\n\n"
              << "Geometry:\n"
              << "  --geo-bind <ini>           Multitarget-tracker geo_bind.ini (image -> ENU)\n"
              << "  --geo-origin <lat,lon>     Shared ENU origin (required for multi-camera)\n"
              << "  --local-dist <m>           Matching threshold in metres (default: 10)\n\n"
              << "ReID:\n"
              << "  --reid-person-model <onnx>  ReID model for pedestrians (e.g. OSNet)\n"
              << "  --reid-vehicle-model <onnx> ReID model for vehicles\n"
              << "  --reid-input <WxH>          DNN ReID input size (default: 128x256)\n"
              << "  --reid-bins <n>             HSV fallback bins per channel (default: 8)\n\n"
              << "MOG2 fallback (used when --detector-settings is absent):\n"
              << "  --history <n>           MOG2 history length (default: 500)\n"
              << "  --var-threshold <x>     MOG2 variance threshold (default: 16)\n"
              << "  --min-area <px>         Minimum foreground contour area (default: automatic)\n\n"
              << "  --version               Print version and exit\n"
              << "  -h, --help              Print this help and exit\n";
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
        } else if (arg == "--start-frame") {
            auto value = nextValue("--start-frame");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.startFrame = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --start-frame: " + *value;
                return std::nullopt;
            }
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
        } else if (arg == "--start-epoch-ms") {
            auto value = nextValue("--start-epoch-ms");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.startEpochMs = std::stoll(*value);
            } catch (const std::exception&) {
                error = "invalid value for --start-epoch-ms: " + *value;
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
        } else if (arg == "--detector-settings") {
            auto value = nextValue("--detector-settings");
            if (!value) {
                return std::nullopt;
            }
            options.detectorSettings = std::move(*value);
        } else if (arg == "--white-list") {
            auto value = nextValue("--white-list");
            if (!value) {
                return std::nullopt;
            }
            options.whiteList = std::move(*value);
        } else if (arg == "--local-dist") {
            auto value = nextValue("--local-dist");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.localCartesianDist = std::stof(*value);
            } catch (const std::exception&) {
                error = "invalid value for --local-dist: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--geo-bind") {
            auto value = nextValue("--geo-bind");
            if (!value) {
                return std::nullopt;
            }
            options.geoBind = std::move(*value);
        } else if (arg == "--geo-origin") {
            auto value = nextValue("--geo-origin");
            if (!value) {
                return std::nullopt;
            }
            options.geoOrigin = std::move(*value);
        } else if (arg == "--reid-person-model") {
            auto value = nextValue("--reid-person-model");
            if (!value) {
                return std::nullopt;
            }
            options.reidPersonModel = std::move(*value);
        } else if (arg == "--reid-vehicle-model") {
            auto value = nextValue("--reid-vehicle-model");
            if (!value) {
                return std::nullopt;
            }
            options.reidVehicleModel = std::move(*value);
        } else if (arg == "--reid-input") {
            auto value = nextValue("--reid-input");
            if (!value) {
                return std::nullopt;
            }
            options.reidInput = std::move(*value);
        } else if (arg == "--reid-bins") {
            auto value = nextValue("--reid-bins");
            if (!value) {
                return std::nullopt;
            }
            try {
                options.reidBins = std::stoi(*value);
            } catch (const std::exception&) {
                error = "invalid value for --reid-bins: " + *value;
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
    if (options.startFrame < 0) {
        error = "--start-frame must not be negative";
        return std::nullopt;
    }
    if (options.reidBins < 1 || options.reidBins > 16) {
        error = "--reid-bins must be in [1, 16]";
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

std::int64_t NowEpochMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string MakeSessionId(std::int64_t epochMs) {
    return "edge-" + std::to_string(epochMs);
}

std::string Trim(std::string text) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

bool ParseSize(const std::string& text, cv::Size& size) {
    const std::size_t separator = text.find_first_of("xX*");
    if (separator == std::string::npos) {
        return false;
    }
    try {
        size.width = std::stoi(text.substr(0, separator));
        size.height = std::stoi(text.substr(separator + 1));
    } catch (const std::exception&) {
        return false;
    }
    return size.width > 0 && size.height > 0;
}

bool ParseLatLon(const std::string& text, double& latitude, double& longitude) {
    const std::size_t separator = text.find(',');
    if (separator == std::string::npos) {
        return false;
    }
    try {
        latitude = std::stod(text.substr(0, separator));
        longitude = std::stod(text.substr(separator + 1));
    } catch (const std::exception&) {
        return false;
    }
    return std::isfinite(latitude) && std::isfinite(longitude) && latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

cv::Point2f BottomCenter(const cv::Rect& rect) {
    return cv::Point2f(static_cast<float>(rect.x) + static_cast<float>(rect.width) * 0.5f,
                       static_cast<float>(rect.y + rect.height));
}

cv::Rect RegionRect(const CRegion& region) {
    if (region.m_brect.area() > 0) {
        return region.m_brect;
    }
    return region.m_rrect.boundingRect();
}

std::unique_ptr<BaseDetector> CreateDetectorFromSettings(const TrackerSettings& settings, const cv::Mat& frame,
                                                         std::string& error) {
    config_t config;
    config.emplace("modelConfiguration", settings.m_nnConfig);
    config.emplace("modelBinary", settings.m_nnWeights);
    config.emplace("confidenceThreshold", std::to_string(settings.m_confidenceThreshold));
    config.emplace("classNames", settings.m_classNames);
    config.emplace("maxCropRatio", std::to_string(settings.m_maxCropRatio));
    config.emplace("maxBatch", std::to_string(settings.m_maxBatch));
    config.emplace("gpuId", std::to_string(settings.m_gpuId));
    config.emplace("net_type", settings.m_netType);
    config.emplace("inference_precision", settings.m_inferencePrecision);
    config.emplace("video_memory", std::to_string(settings.m_maxVideoMemory));
    config.emplace("dnnTarget", settings.m_dnnTarget);
    config.emplace("dnnBackend", settings.m_dnnBackend);
    config.emplace("inWidth", std::to_string(settings.m_inputSize.width));
    config.emplace("inHeight", std::to_string(settings.m_inputSize.height));
    for (const std::string& name : settings.m_whiteList) {
        config.emplace("white_list", name);
    }

    std::cout << "detector: backend=" << settings.m_detectorBackend << ", net_type=" << settings.m_netType
              << ", weights=" << settings.m_nnWeights << ", white_list=";
    for (const std::string& name : settings.m_whiteList) {
        std::cout << name << ';';
    }
    std::cout << '\n';

    auto detector = BaseDetector::CreateDetector(static_cast<tracking::Detectors>(settings.m_detectorBackend), config,
                                                 frame.getUMat(cv::ACCESS_READ));
    if (!detector) {
        error = "cannot create the detector";
    }
    return detector;
}

TrackerSettings MakeTrackerSettings(const Options& options, const TrackerSettings* base, const cv::Mat& frame,
                                    double fps, bool hasGeoBinding) {
    TrackerSettings settings;
    if (base != nullptr) {
        settings = *base;
    } else {
        settings.m_kalmanType = tracking::KalmanLinear;
        settings.m_filterGoal = tracking::FilterCenter;
        settings.m_lostTrackType = tracking::TrackNone;
        settings.m_matchType = tracking::MatchHungrian;
        settings.m_distThres = 0.95f;
        settings.m_maximumAllowedLostTime = 10.0;
        settings.m_maxTraceLength = 4.0;
        settings.SetDistance(hasGeoBinding ? tracking::DistLocalCartesian : tracking::DistRects);
    }

    // Multitarget-tracker measures Kalman time in seconds.
    settings.m_dt = static_cast<float>(1.0 / fps);
    settings.m_minAreaRadiusPix = static_cast<track_t>(frame.rows) / 20.f;

    if (hasGeoBinding) {
        settings.SetDistance(tracking::DistLocalCartesian);
        settings.m_distThresLocalCartesian = std::max(0.1f, options.localCartesianDist);
    }

    settings.AddNearTypes(TypeConverter::Str2Type("car"), TypeConverter::Str2Type("bus"), false);
    settings.AddNearTypes(TypeConverter::Str2Type("car"), TypeConverter::Str2Type("truck"), false);
    settings.AddNearTypes(TypeConverter::Str2Type("person"), TypeConverter::Str2Type("bicycle"), true);
    settings.AddNearTypes(TypeConverter::Str2Type("person"), TypeConverter::Str2Type("motorbike"), true);
    return settings;
}

std::unique_ptr<tmcmt::IReIdExtractor> CreateReIdExtractor(const Options& options, std::string& error) {
    constexpr int kMinRoiSize = 8;
    if (!options.reidPersonModel.empty() || !options.reidVehicleModel.empty()) {
        tmcmt::TypeAwareReIdOptions reidOptions;
        reidOptions.person_model_path = options.reidPersonModel;
        reidOptions.vehicle_model_path = options.reidVehicleModel;
        reidOptions.fallback_bins = options.reidBins;
        reidOptions.min_roi_size = kMinRoiSize;
        if (!options.reidInput.empty() && !ParseSize(options.reidInput, reidOptions.dnn_input_size)) {
            error = "invalid --reid-input '" + options.reidInput + "', expected WxH";
            return nullptr;
        }
        auto created = tmcmt::TypeAwareReIdExtractor::Create(std::move(reidOptions));
        if (!created) {
            error = created.status().ToString();
            return nullptr;
        }
        return std::move(created).value();
    }

    tmcmt::ColorHistogramOptions reidOptions;
    reidOptions.bins = options.reidBins;
    reidOptions.model_id = 1;
    reidOptions.min_roi_size = kMinRoiSize;
    return std::make_unique<tmcmt::ColorHistogramReIdExtractor>(reidOptions);
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

    if (options->startFrame > 0) {
        capture.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(options->startFrame));
    }

    if (!options->formatExplicit) {
        options->format = DetectFormatFromPath(options->outputPath);
    }
    if (options->startEpochMs <= 0) {
        options->startEpochMs = NowEpochMs();
    }
    if (options->sessionId.empty()) {
        options->sessionId = MakeSessionId(options->startEpochMs);
    }

    std::optional<tmcmt::GeoBinding> geoBinding;
    if (!options->geoBind.empty()) {
        tmcmt::GeoBindingOptions geoOptions;
        if (!options->geoOrigin.empty()) {
            if (!ParseLatLon(options->geoOrigin, geoOptions.origin_latitude, geoOptions.origin_longitude)) {
                std::cerr << "error: invalid --geo-origin '" << options->geoOrigin
                          << "', expected 'lat,lon'\n";
                return kExitRuntimeError;
            }
            geoOptions.has_origin = true;
        }
        auto loaded = tmcmt::GeoBinding::LoadFromFile(options->geoBind, geoOptions);
        if (!loaded) {
            std::cerr << "error: " << loaded.status().ToString() << '\n';
            return kExitRuntimeError;
        }
        geoBinding = std::move(loaded).value();
        std::cout << "geometry: " << geoBinding->Describe() << '\n';
    }

    std::string reidError;
    std::unique_ptr<tmcmt::IReIdExtractor> extractor = CreateReIdExtractor(*options, reidError);
    if (!extractor) {
        std::cerr << "error: " << reidError << '\n';
        return kExitRuntimeError;
    }
    std::cout << "reid: " << extractor->Describe() << '\n';

    std::optional<TrackerSettings> mttSettings;
    if (!options->detectorSettings.empty()) {
        TrackerSettings parsed;
        if (!ParseTrackerSettings(options->detectorSettings, parsed)) {
            std::cerr << "error: cannot parse detector settings: " << options->detectorSettings << '\n';
            return kExitRuntimeError;
        }
        if (!options->whiteList.empty()) {
            parsed.m_whiteList.clear();
            std::istringstream stream(options->whiteList);
            std::string name;
            while (std::getline(stream, name, ';')) {
                name = Trim(name);
                if (!name.empty()) {
                    parsed.m_whiteList.push_back(name);
                }
            }
        }
        mttSettings = std::move(parsed);
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

    std::unique_ptr<BaseDetector> detector;
    std::unique_ptr<BaseTracker> tracker;
    const bool hasGeoBinding = geoBinding.has_value();

    tmcmt::MttAdapterOptions adapterOptions;
    adapterOptions.min_confidence = options->minConfidence;
    adapterOptions.min_trace_length = options->minTraceLength;
    tmcmt::MttTrackAdapter adapter(adapterOptions);

    tmcmt::EdgeExportConfig exportConfig;
    exportConfig.camera_id = options->cameraId;
    exportConfig.session_id = options->sessionId;
    exportConfig.chunk_interval_ms = options->chunkIntervalMs;
    exportConfig.model_id = extractor->ModelId();
    tmcmt::EdgeTrackExporter exporter(exportConfig);

    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::Mat frame;
    cv::Mat foreground;
    std::vector<std::vector<cv::Point>> contours;
    std::vector<tmcmt::Tracklet> chunks;

    std::size_t frames = 0;
    std::size_t chunksWritten = 0;
    std::size_t observationsWritten = 0;
    std::size_t worldValid = 0;
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
            for (const tmcmt::Observation& observation : chunk.observations) {
                if (observation.world.valid) {
                    ++worldValid;
                }
            }
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

        if (!options->detectorSettings.empty() && !detector) {
            std::string detectorError;
            detector = CreateDetectorFromSettings(*mttSettings, frame, detectorError);
            if (!detector) {
                std::cerr << "error: " << detectorError << '\n';
                return kExitRuntimeError;
            }
        }
        if (!tracker) {
            const TrackerSettings settings =
                MakeTrackerSettings(*options, mttSettings ? &*mttSettings : nullptr, frame, fps, hasGeoBinding);
            tracker = BaseTracker::CreateTracker(settings, static_cast<float>(fps));
            if (!tracker) {
                std::cerr << "error: cannot create Multitarget-tracker instance\n";
                return kExitRuntimeError;
            }
        }

        regions_t regions;
        if (detector) {
            detector->Detect(frame.getUMat(cv::ACCESS_READ));
            regions = detector->GetDetects();
        } else {
            if (minArea <= 0) {
                minArea = std::max(64, (frame.cols * frame.rows) / 20000);
            }
            mog2->apply(frame, foreground);
            cv::threshold(foreground, foreground, 200, 255, cv::THRESH_BINARY);
            cv::morphologyEx(foreground, foreground, cv::MORPH_OPEN, kernel);
            cv::morphologyEx(foreground, foreground, cv::MORPH_CLOSE, kernel);

            contours.clear();
            cv::findContours(foreground, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            regions.reserve(contours.size());
            for (const auto& contour : contours) {
                const cv::Rect rect = cv::boundingRect(contour);
                if (rect.area() < minArea) {
                    continue;
                }
                regions.emplace_back(rect);
            }
        }

        if (geoBinding) {
            for (CRegion& region : regions) {
                const cv::Rect rect = RegionRect(region);
                const tmcmt::WorldPoint world = geoBinding->Project(BottomCenter(rect), frame.size());
                if (world.valid) {
                    region.SetLocalCartesian(cv::Point2d(world.x, world.y));
                }
            }
        }

        const auto frameOffsetMs =
            static_cast<std::int64_t>(std::llround(static_cast<double>(options->startFrame + frames) * 1000.0 / fps));
        const std::int64_t timestampMs = options->startEpochMs + frameOffsetMs;
        const time_point_t nowPoint = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
        const tmcmt::Timestamp now = std::chrono::milliseconds(timestampMs);

        tracker->UpdateMat(regions, frame, nowPoint);

        const std::vector<TrackingObject> tracks = tracker->GetTracksCopy();
        for (const TrackingObject& track : tracks) {
            if (!adapter.ShouldExport(track)) {
                continue;
            }

            auto observation = adapter.ToObservation(track, now);
            if (!observation) {
                continue;
            }

            if (geoBinding) {
                const cv::Rect rect = track.m_rrect.boundingRect();
                observation.value().world = geoBinding->Project(BottomCenter(rect), frame.size());
            }

            const tmcmt::BBox bbox = observation.value().bbox;
            cv::Rect roi(cvRound(bbox.x), cvRound(bbox.y), cvRound(bbox.w), cvRound(bbox.h));
            roi &= cv::Rect(0, 0, frame.cols, frame.rows);
            if (roi.width < 8 || roi.height < 8) {
                continue;
            }

            const tmcmt::ObjectType type = tmcmt::MttTrackAdapter::ToObjectType(track.m_type);
            auto embedding = extractor->Extract(frame, roi, type);
            if (!embedding) {
                ++reidFailures;
                continue;
            }

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
              << "time_base_ms: " << options->startEpochMs << '\n'
              << "start_frame: " << options->startFrame << '\n'
              << "tracklet_chunks: " << chunksWritten << '\n'
              << "observations: " << observationsWritten << '\n'
              << "world_valid: " << worldValid << '\n'
              << "reid_failures: " << reidFailures << '\n';
    return kExitOk;
}
