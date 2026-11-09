#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/io/FileTrackletSource.h"
#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/TransitionEstimator.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitDataError = 3;

struct Options {
    std::string tracklets;
    std::string pairs;
    std::string out;
    tmcmt::TrackletFileFormat format = tmcmt::TrackletFileFormat::Auto;
    std::size_t minSamples = 3;
    double madScale = 1.4826;
    double minStddevMs = 1.0;
    double outlierSigma = 3.0;
    bool includeSameCamera = false;
    std::int64_t fallbackMinMs = 0;
    std::int64_t fallbackMaxMs = 120000;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT transition estimator " << tmcmt::kVersion << "\n\n"
              << "Usage: " << program
              << " --tracklets <file> --pairs <file> --out <camera_links.yaml> [options]\n\n"
              << "Options:\n"
              << "  --tracklets <path>     Tracklet stream (JSONL/protobuf)\n"
              << "  --pairs <path>         JSONL with {\"from\": <id>, \"to\": <id>} records\n"
              << "  --out <path>           Output camera link model (YAML)\n"
              << "  --format <fmt>         auto|jsonl|protobuf (default: auto)\n"
              << "  --min-samples <n>      Minimum samples per camera pair (default: 3)\n"
              << "  --mad-scale <x>        MAD -> sigma factor (default: 1.4826)\n"
              << "  --min-stddev-ms <x>    Lower clamp for sigma (default: 1.0)\n"
              << "  --outlier-sigma <x>    One-pass trim threshold; 0 disables (default: 3.0)\n"
              << "  --include-same-camera  Keep same-camera samples (default: off)\n"
              << "  --fallback-min-ms <n>  Fallback window start (default: 0)\n"
              << "  --fallback-max-ms <n>  Fallback window end (default: 120000)\n"
              << "  --version              Print version and exit\n"
              << "  -h, --help             Print this help and exit\n\n"
              << "Pair record:\n"
              << "  {\"from\": 123456789, \"to\": 987654321}\n"
              << "The ids must exist in the tracklet stream; camera ids and begin/end\n"
              << "timestamps are taken from the tracklets themselves.\n";
}

template <typename T>
bool ParseUnsigned(const std::string& text, T& out) {
    try {
        out = static_cast<T>(std::stoull(text));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool ParseDouble(const std::string& text, double& out) {
    try {
        std::size_t consumed = 0;
        out = std::stod(text, &consumed);
        return consumed == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool ParseInt64(const std::string& text, std::int64_t& out) {
    try {
        std::size_t consumed = 0;
        out = std::stoll(text, &consumed);
        return consumed == text.size();
    } catch (const std::exception&) {
        return false;
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
        } else if (arg == "--include-same-camera") {
            options.includeSameCamera = true;
        } else if (arg == "--tracklets") {
            auto value = nextValue("--tracklets");
            if (!value) {
                return std::nullopt;
            }
            options.tracklets = *value;
        } else if (arg == "--pairs") {
            auto value = nextValue("--pairs");
            if (!value) {
                return std::nullopt;
            }
            options.pairs = *value;
        } else if (arg == "--out") {
            auto value = nextValue("--out");
            if (!value) {
                return std::nullopt;
            }
            options.out = *value;
        } else if (arg == "--format") {
            auto value = nextValue("--format");
            if (!value) {
                return std::nullopt;
            }
            if (*value == "auto") {
                options.format = tmcmt::TrackletFileFormat::Auto;
            } else if (*value == "jsonl") {
                options.format = tmcmt::TrackletFileFormat::Jsonl;
            } else if (*value == "protobuf") {
                options.format = tmcmt::TrackletFileFormat::Protobuf;
            } else {
                error = "unknown format '" + *value + "'";
                return std::nullopt;
            }
        } else if (arg == "--min-samples") {
            auto value = nextValue("--min-samples");
            if (!value || !ParseUnsigned(*value, options.minSamples) || options.minSamples == 0) {
                error = "invalid --min-samples";
                return std::nullopt;
            }
        } else if (arg == "--mad-scale") {
            auto value = nextValue("--mad-scale");
            if (!value || !ParseDouble(*value, options.madScale) || !(options.madScale > 0.0)) {
                error = "invalid --mad-scale";
                return std::nullopt;
            }
        } else if (arg == "--min-stddev-ms") {
            auto value = nextValue("--min-stddev-ms");
            if (!value || !ParseDouble(*value, options.minStddevMs) || !(options.minStddevMs > 0.0)) {
                error = "invalid --min-stddev-ms";
                return std::nullopt;
            }
        } else if (arg == "--outlier-sigma") {
            auto value = nextValue("--outlier-sigma");
            if (!value || !ParseDouble(*value, options.outlierSigma) || options.outlierSigma < 0.0) {
                error = "invalid --outlier-sigma";
                return std::nullopt;
            }
        } else if (arg == "--fallback-min-ms") {
            auto value = nextValue("--fallback-min-ms");
            if (!value || !ParseInt64(*value, options.fallbackMinMs) || options.fallbackMinMs < 0) {
                error = "invalid --fallback-min-ms";
                return std::nullopt;
            }
        } else if (arg == "--fallback-max-ms") {
            auto value = nextValue("--fallback-max-ms");
            if (!value || !ParseInt64(*value, options.fallbackMaxMs)) {
                error = "invalid --fallback-max-ms";
                return std::nullopt;
            }
        } else {
            error = "unknown option '" + arg + "'";
            return std::nullopt;
        }
    }

    if (options.showHelp || options.showVersion) {
        return options;
    }
    if (options.tracklets.empty()) {
        error = "--tracklets is required";
        return std::nullopt;
    }
    if (options.pairs.empty()) {
        error = "--pairs is required";
        return std::nullopt;
    }
    if (options.out.empty()) {
        error = "--out is required";
        return std::nullopt;
    }
    if (options.fallbackMaxMs <= options.fallbackMinMs) {
        error = "--fallback-max-ms must be greater than --fallback-min-ms";
        return std::nullopt;
    }
    return options;
}

bool LoadTracklets(const std::string& path, tmcmt::TrackletFileFormat format,
                   std::unordered_map<tmcmt::TrackletId, tmcmt::Tracklet>& byId,
                   std::string& error) {
    tmcmt::FileTrackletSource::Options sourceOptions;
    sourceOptions.format = format;
    sourceOptions.speed = 0.0;

    auto source = tmcmt::FileTrackletSource::Open(path, sourceOptions);
    if (!source) {
        error = source.status().ToString();
        return false;
    }

    tmcmt::Tracklet tracklet;
    for (;;) {
        auto next = source.value()->Next(tracklet);
        if (!next) {
            error = next.status().ToString();
            return false;
        }
        if (!next.value()) {
            break;
        }
        byId[tracklet.id] = tracklet;
    }
    return true;
}

bool ExtractId(const std::string& line, std::string_view key, tmcmt::TrackletId& out) {
    const std::string token = "\"" + std::string(key) + "\"";
    std::size_t position = line.find(token);
    if (position == std::string::npos) {
        return false;
    }
    position = line.find(':', position + token.size());
    if (position == std::string::npos) {
        return false;
    }
    ++position;
    while (position < line.size() && std::isspace(static_cast<unsigned char>(line[position])) != 0) {
        ++position;
    }
    std::size_t end = position;
    while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])) != 0) {
        ++end;
    }
    if (end == position) {
        return false;
    }
    try {
        out = static_cast<tmcmt::TrackletId>(std::stoull(line.substr(position, end - position)));
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool LoadPairs(const std::string& path, std::vector<std::pair<tmcmt::TrackletId, tmcmt::TrackletId>>& pairs,
               std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot open pairs '" + path + "'";
        return false;
    }

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (lineNumber == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        if (line[0] == '#') {
            continue;
        }

        tmcmt::TrackletId from = 0;
        tmcmt::TrackletId to = 0;
        if (!ExtractId(line, "from", from) || !ExtractId(line, "to", to)) {
            error = "pairs line " + std::to_string(lineNumber) +
                    ": expected numeric \"from\" and \"to\" ids";
            return false;
        }
        pairs.emplace_back(from, to);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string error;
    const std::optional<Options> parsed = ParseArgs(argc, argv, error);
    if (!parsed) {
        std::cerr << "error: " << error << "\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_st_estimate");
        return kExitUsageError;
    }
    const Options& options = *parsed;
    if (options.showHelp) {
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_st_estimate");
        return kExitOk;
    }
    if (options.showVersion) {
        std::cout << tmcmt::kVersion << "\n";
        return kExitOk;
    }

    std::unordered_map<tmcmt::TrackletId, tmcmt::Tracklet> tracklets;
    if (!LoadTracklets(options.tracklets, options.format, tracklets, error)) {
        std::cerr << "error: " << error << "\n";
        return kExitDataError;
    }

    std::vector<std::pair<tmcmt::TrackletId, tmcmt::TrackletId>> pairs;
    if (!LoadPairs(options.pairs, pairs, error)) {
        std::cerr << "error: " << error << "\n";
        return kExitDataError;
    }

    std::vector<tmcmt::st::TransitionSample> samples;
    samples.reserve(pairs.size());
    std::size_t missing = 0;
    for (const auto& [fromId, toId] : pairs) {
        const auto fromIt = tracklets.find(fromId);
        const auto toIt = tracklets.find(toId);
        if (fromIt == tracklets.end() || toIt == tracklets.end()) {
            ++missing;
            continue;
        }
        tmcmt::st::TransitionSample sample;
        sample.from = fromIt->second.camera;
        sample.to = toIt->second.camera;
        sample.from_end = fromIt->second.end;
        sample.to_begin = toIt->second.begin;
        samples.push_back(std::move(sample));
    }

    tmcmt::st::EstimatorOptions estimatorOptions;
    estimatorOptions.min_samples = options.minSamples;
    estimatorOptions.mad_scale = options.madScale;
    estimatorOptions.min_stddev_ms = options.minStddevMs;
    estimatorOptions.outlier_sigma = options.outlierSigma;
    estimatorOptions.include_same_camera = options.includeSameCamera;

    auto links = tmcmt::st::TransitionEstimator::Estimate(samples, estimatorOptions);
    if (!links) {
        std::cerr << "error: " << links.status().ToString() << "\n";
        return kExitDataError;
    }

    tmcmt::st::CameraLinkModel model;
    model.SetDefaultLinked(false);
    model.SetFallback(tmcmt::st::TransitionWindow{options.fallbackMinMs, options.fallbackMaxMs});
    for (const tmcmt::st::CameraLink& link : links.value()) {
        if (tmcmt::Status status = model.SetLink(link); !status.ok()) {
            std::cerr << "error: " << status.ToString() << "\n";
            return kExitDataError;
        }
    }

    if (tmcmt::Status status = model.Save(options.out); !status.ok()) {
        std::cerr << "error: " << status.ToString() << "\n";
        return kExitDataError;
    }

    std::cout << "tracklets: " << tracklets.size() << "\n"
              << "pairs: " << pairs.size() << " (missing tracklets: " << missing << ")\n"
              << "samples: " << samples.size() << "\n"
              << "links: " << links.value().size() << "\n"
              << "wrote: " << options.out << "\n";
    return kExitOk;
}
