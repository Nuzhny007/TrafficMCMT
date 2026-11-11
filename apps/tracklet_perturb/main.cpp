#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/io/FileTrackletSource.h"
#include "trafficmcmt/io/TrackletSinks.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitData = 3;

struct Options {
    std::string input;
    std::string output;
    /// Maximum absolute displacement of the delivery order, in stream
    /// milliseconds. Event timestamps are preserved.
    std::int64_t jitterMs = 0;
    /// Additional per-message event-time noise. This corrupts the timestamps
    /// themselves and is meant as a worst-case stress test, not as the DoD
    /// out-of-order scenario.
    std::int64_t eventJitterMs = 0;
    double reorder = 0.0;
    std::uint64_t seed = 42;
    tmcmt::TrackletFileFormat format = tmcmt::TrackletFileFormat::Auto;
    bool showHelp = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT tracklet perturbator " << tmcmt::kVersion << "\n\n"
              << "Usage: " << program << " --in <path> --out <path> [options]\n\n"
              << "Options:\n"
              << "  --in <path>          Input tracklets (JSONL or protobuf)\n"
              << "  --out <path>         Output tracklets\n"
              << "  --jitter-ms <n>      Delivery-order jitter: each message may be delayed or\n"
              << "                       advanced by up to n ms of stream time. Event timestamps\n"
              << "                       are not modified; only the arrival order changes.\n"
              << "  --event-jitter-ms <n>  Stress mode: shift event timestamps by [-n, +n] ms\n"
              << "  --reorder <fraction> Probability of swapping a tracklet with its successor (0..1)\n"
              << "  --seed <n>           RNG seed; the same seed produces the same file\n"
              << "  --format <fmt>       Output format: auto|jsonl|protobuf (default: by extension)\n"
              << "  -h, --help           Print this help and exit\n";
}

std::optional<std::int64_t> ParseInt(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const long long value = std::stoll(text, &consumed);
        if (consumed != text.size()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> ParseDouble(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const double value = std::stod(text, &consumed);
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
        const auto next = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                error = std::string("missing value for ") + name;
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        } else if (arg == "--in") {
            auto value = next("--in");
            if (!value) {
                return std::nullopt;
            }
            options.input = std::move(*value);
        } else if (arg == "--out") {
            auto value = next("--out");
            if (!value) {
                return std::nullopt;
            }
            options.output = std::move(*value);
        } else if (arg == "--jitter-ms") {
            auto value = next("--jitter-ms");
            if (!value) {
                return std::nullopt;
            }
            const auto parsed = ParseInt(*value);
            if (!parsed || *parsed < 0) {
                error = "invalid value for --jitter-ms: '" + *value + "'";
                return std::nullopt;
            }
            options.jitterMs = *parsed;
        } else if (arg == "--event-jitter-ms") {
            auto value = next("--event-jitter-ms");
            if (!value) {
                return std::nullopt;
            }
            const auto parsed = ParseInt(*value);
            if (!parsed || *parsed < 0) {
                error = "invalid value for --event-jitter-ms: '" + *value + "'";
                return std::nullopt;
            }
            options.eventJitterMs = *parsed;
        } else if (arg == "--reorder") {
            auto value = next("--reorder");
            if (!value) {
                return std::nullopt;
            }
            const auto parsed = ParseDouble(*value);
            if (!parsed || *parsed < 0.0 || *parsed > 1.0) {
                error = "invalid value for --reorder: '" + *value + "'";
                return std::nullopt;
            }
            options.reorder = *parsed;
        } else if (arg == "--seed") {
            auto value = next("--seed");
            if (!value) {
                return std::nullopt;
            }
            const auto parsed = ParseInt(*value);
            if (!parsed || *parsed < 0) {
                error = "invalid value for --seed: '" + *value + "'";
                return std::nullopt;
            }
            options.seed = static_cast<std::uint64_t>(*parsed);
        } else if (arg == "--format") {
            auto value = next("--format");
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
                error = "unknown format '" + *value + "', expected auto|jsonl|protobuf";
                return std::nullopt;
            }
        } else {
            error = "unknown argument '" + arg + "'";
            return std::nullopt;
        }
    }
    return options;
}

tmcmt::TrackletFileFormat ResolveOutputFormat(const Options& options) {
    if (options.format != tmcmt::TrackletFileFormat::Auto) {
        return options.format;
    }
    const std::string& path = options.output;
    const auto endsWith = [&path](std::string_view suffix) {
        if (path.size() < suffix.size()) {
            return false;
        }
        return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (endsWith(".bin") || endsWith(".pb") || endsWith(".protobuf") || endsWith(".tracklets")) {
        return tmcmt::TrackletFileFormat::Protobuf;
    }
    return tmcmt::TrackletFileFormat::Jsonl;
}

} // namespace

int main(int argc, char** argv) {
    std::string error;
    std::optional<Options> options = ParseArgs(argc, argv, error);
    if (!options) {
        std::cerr << "error: " << error << "\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_tracklet_perturb");
        return kExitUsage;
    }
    if (options->showHelp) {
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_tracklet_perturb");
        return kExitOk;
    }
    if (options->input.empty() || options->output.empty()) {
        std::cerr << "error: --in and --out are required\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_tracklet_perturb");
        return kExitUsage;
    }

    tmcmt::FileTrackletSource::Options sourceOptions;
    sourceOptions.speed = 0.0;
    auto source = tmcmt::FileTrackletSource::Open(options->input, sourceOptions);
    if (!source) {
        std::cerr << "error: " << source.status().ToString() << "\n";
        return kExitData;
    }

    std::vector<tmcmt::Tracklet> tracklets;
    for (;;) {
        tmcmt::Tracklet tracklet;
        auto next = source.value()->Next(tracklet);
        if (!next) {
            std::cerr << "error: " << next.status().ToString() << "\n";
            return kExitData;
        }
        if (!next.value()) {
            break;
        }
        tracklets.push_back(std::move(tracklet));
    }

    std::mt19937_64 rng(options->seed);
    std::size_t shifted = 0;

    // Optional worst-case stress mode: corrupt the event timestamps themselves.
    if (options->eventJitterMs > 0) {
        std::uniform_int_distribution<std::int64_t> jitter(-options->eventJitterMs, options->eventJitterMs);
        for (tmcmt::Tracklet& tracklet : tracklets) {
            const std::int64_t delta = jitter(rng);
            if (delta == 0) {
                continue;
            }
            tracklet.begin += tmcmt::Timestamp(delta);
            tracklet.end += tmcmt::Timestamp(delta);
            for (tmcmt::Observation& observation : tracklet.observations) {
                observation.ts += tmcmt::Timestamp(delta);
            }
            ++shifted;
        }
    }

    // Delivery-order jitter: perturb only the position of each message in the
    // stream. The ordering key is the event time plus a bounded random
    // displacement, so a message can arrive up to `jitterMs` early or late
    // relative to its event time. The event timestamps stay untouched, which
    // is exactly the out-of-order stress the Master's timestamp buffer must
    // absorb.
    std::vector<std::size_t> order(tracklets.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::size_t displaced = 0;
    if (options->jitterMs > 0 && tracklets.size() > 1) {
        std::uniform_int_distribution<std::int64_t> jitter(-options->jitterMs, options->jitterMs);
        std::vector<std::pair<std::int64_t, std::size_t>> keys;
        keys.reserve(tracklets.size());
        for (std::size_t i = 0; i < tracklets.size(); ++i) {
            const std::int64_t delta = jitter(rng);
            if (delta != 0) {
                ++displaced;
            }
            keys.emplace_back(tracklets[i].begin.count() + delta, i);
        }
        std::stable_sort(keys.begin(), keys.end(),
                         [](const std::pair<std::int64_t, std::size_t>& lhs,
                            const std::pair<std::int64_t, std::size_t>& rhs) {
                             if (lhs.first != rhs.first) {
                                 return lhs.first < rhs.first;
                             }
                             return lhs.second < rhs.second;
                         });
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = keys[i].second;
        }
    }

    std::size_t swapped = 0;
    if (options->reorder > 0.0 && order.size() > 1) {
        std::uniform_real_distribution<double> coin(0.0, 1.0);
        for (std::size_t i = 0; i + 1 < order.size(); ++i) {
            if (coin(rng) < options->reorder) {
                std::swap(order[i], order[i + 1]);
                ++swapped;
            }
        }
    }

    const tmcmt::TrackletFileFormat format = ResolveOutputFormat(*options);
    tmcmt::Status status;
    const auto writeAll = [&](auto& sink) -> tmcmt::Status {
        for (const std::size_t index : order) {
            tmcmt::Status written = sink->Write(tracklets[index]);
            if (!written.ok()) {
                return written;
            }
        }
        return sink->Flush();
    };

    if (format == tmcmt::TrackletFileFormat::Protobuf) {
        auto sink = tmcmt::BinaryTrackletSink::Open(options->output);
        if (!sink) {
            std::cerr << "error: " << sink.status().ToString() << "\n";
            return kExitData;
        }
        status = writeAll(sink.value());
    } else {
        auto sink = tmcmt::JsonlTrackletSink::Open(options->output);
        if (!sink) {
            std::cerr << "error: " << sink.status().ToString() << "\n";
            return kExitData;
        }
        status = writeAll(sink.value());
    }
    if (!status.ok()) {
        std::cerr << "error: " << status.ToString() << "\n";
        return kExitData;
    }

    std::cout << "read=" << tracklets.size() << " displaced=" << displaced << " reordered=" << swapped
              << " event_jittered=" << shifted << " jitter_ms=" << options->jitterMs
              << " event_jitter_ms=" << options->eventJitterMs << " reorder=" << options->reorder
              << " seed=" << options->seed << "\n";
    return kExitOk;
}
