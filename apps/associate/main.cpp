#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/flow/FlowAssociator.h"
#include "trafficmcmt/io/FileTrackletSource.h"
#include "trafficmcmt/metrics/MotMetrics.h"
#include "trafficmcmt/reid/GalleryManager.h"
#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/PairCandidateGenerator.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitDataError = 3;

struct Options {
    std::string tracklets;
    std::string cameraLinks;
    std::string out;
    std::string gt;
    tmcmt::TrackletFileFormat format = tmcmt::TrackletFileFormat::Auto;
    tmcmt::SolverKind solver = tmcmt::SolverKind::Ssp;
    bool json = false;
    double linkReward = 1.0;
    std::size_t maxCandidates = 15;
    std::size_t maxComponentNodes = 20000;
    std::size_t topK = 20;
    double minSimilarity = 0.0;
    double maxTotalCost = 1.0;
    std::int64_t commitLagMs = 15000;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT offline association " << tmcmt::kVersion << "\n\n"
              << "Usage: " << program
              << " --tracklets <file> --out <file> [options]\n\n"
              << "Options:\n"
              << "  --tracklets <path>       Tracklet stream (JSONL/protobuf)\n"
              << "  --camera-links <path>    Camera topology/transition model (YAML)\n"
              << "  --out <path>             Output assignments (JSONL)\n"
              << "  --gt <path>              Ground truth identities (JSONL) for metrics\n"
              << "  --format <fmt>           auto|jsonl|protobuf (default: auto)\n"
              << "  --solver <name>          ssp|mussp|greedy (default: ssp)\n"
              << "  --link-reward <x>        Transition cost is c_total - x (default: 1.0)\n"
              << "  --max-candidates <n>     Top-K outgoing transitions per node (default: 15)\n"
              << "  --max-component-nodes <n> Skip larger components (default: 20000)\n"
              << "  --top-k <n>              st-ReID candidates per query (default: 20)\n"
              << "  --min-similarity <x>     Appearance threshold (default: 0.0)\n"
              << "  --max-total-cost <x>     Reject pairs above c_total (default: 1.0)\n"
              << "  --commit-lag-ms <n>      GlobalIdManager commit lag (default: 15000)\n"
              << "  --json                   Print a JSON summary\n"
              << "  --version                Print version and exit\n"
              << "  -h, --help               Print this help and exit\n\n"
              << "Output record:\n"
              << "  {\"tracklet\": 123, \"global_id\": 1, \"camera\": \"cam-1\", "
                 "\"begin_ms\": 0, \"end_ms\": 1000}\n"
              << "Ground-truth record:\n"
              << "  {\"tracklet\": 123, \"identity\": \"object-7\"}\n";
}

template <typename T>
bool ParseUnsigned(const std::string& text, T& out) {
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        out = static_cast<T>(value);
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

bool ParseSolver(const std::string& text, tmcmt::SolverKind& out) {
    if (text == "ssp") {
        out = tmcmt::SolverKind::Ssp;
        return true;
    }
    if (text == "mussp") {
        out = tmcmt::SolverKind::MuSsp;
        return true;
    }
    if (text == "greedy") {
        out = tmcmt::SolverKind::Greedy;
        return true;
    }
    return false;
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
            options.json = true;
        } else if (arg == "--tracklets") {
            auto value = nextValue("--tracklets");
            if (!value) {
                return std::nullopt;
            }
            options.tracklets = *value;
        } else if (arg == "--camera-links") {
            auto value = nextValue("--camera-links");
            if (!value) {
                return std::nullopt;
            }
            options.cameraLinks = *value;
        } else if (arg == "--out") {
            auto value = nextValue("--out");
            if (!value) {
                return std::nullopt;
            }
            options.out = *value;
        } else if (arg == "--gt") {
            auto value = nextValue("--gt");
            if (!value) {
                return std::nullopt;
            }
            options.gt = *value;
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
        } else if (arg == "--solver") {
            auto value = nextValue("--solver");
            if (!value || !ParseSolver(*value, options.solver)) {
                error = "invalid --solver (expected ssp|mussp|greedy)";
                return std::nullopt;
            }
        } else if (arg == "--link-reward") {
            auto value = nextValue("--link-reward");
            if (!value || !ParseDouble(*value, options.linkReward) ||
                !(options.linkReward > 0.0)) {
                error = "invalid --link-reward";
                return std::nullopt;
            }
        } else if (arg == "--max-candidates") {
            auto value = nextValue("--max-candidates");
            if (!value || !ParseUnsigned(*value, options.maxCandidates) ||
                options.maxCandidates == 0) {
                error = "invalid --max-candidates";
                return std::nullopt;
            }
        } else if (arg == "--max-component-nodes") {
            auto value = nextValue("--max-component-nodes");
            if (!value || !ParseUnsigned(*value, options.maxComponentNodes) ||
                options.maxComponentNodes == 0) {
                error = "invalid --max-component-nodes";
                return std::nullopt;
            }
        } else if (arg == "--top-k") {
            auto value = nextValue("--top-k");
            if (!value || !ParseUnsigned(*value, options.topK) || options.topK == 0) {
                error = "invalid --top-k";
                return std::nullopt;
            }
        } else if (arg == "--min-similarity") {
            auto value = nextValue("--min-similarity");
            if (!value || !ParseDouble(*value, options.minSimilarity) ||
                options.minSimilarity < -1.0 || options.minSimilarity > 1.0) {
                error = "invalid --min-similarity";
                return std::nullopt;
            }
        } else if (arg == "--max-total-cost") {
            auto value = nextValue("--max-total-cost");
            if (!value || !ParseDouble(*value, options.maxTotalCost) ||
                options.maxTotalCost < 0.0) {
                error = "invalid --max-total-cost";
                return std::nullopt;
            }
        } else if (arg == "--commit-lag-ms") {
            auto value = nextValue("--commit-lag-ms");
            if (!value || !ParseInt64(*value, options.commitLagMs) || options.commitLagMs < 0) {
                error = "invalid --commit-lag-ms";
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
    if (options.out.empty()) {
        error = "--out is required";
        return std::nullopt;
    }
    return options;
}

bool LoadTracklets(const std::string& path, tmcmt::TrackletFileFormat format,
                   std::vector<tmcmt::Tracklet>& tracklets,
                   std::unordered_map<tmcmt::TrackletId, std::size_t>& index,
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
        if (index.find(tracklet.id) != index.end()) {
            error = "duplicate tracklet id " + std::to_string(tracklet.id);
            return false;
        }
        index.emplace(tracklet.id, tracklets.size());
        tracklets.push_back(tracklet);
    }
    return true;
}

bool LoadGroundTruth(const std::string& path,
                     std::unordered_map<tmcmt::TrackletId, std::string>& identities,
                     std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot open ground truth '" + path + "'";
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

        try {
            const YAML::Node node = YAML::Load(line);
            if (!node.IsMap()) {
                error = "ground truth line " + std::to_string(lineNumber) +
                        ": expected a JSON object";
                return false;
            }
            const YAML::Node idNode = node["tracklet"] ? node["tracklet"] : node["id"];
            const YAML::Node identityNode =
                node["identity"] ? node["identity"] : (node["gt"] ? node["gt"] : node["label"]);
            if (!idNode || !idNode.IsScalar() || !identityNode || !identityNode.IsScalar()) {
                error = "ground truth line " + std::to_string(lineNumber) +
                        ": expected numeric tracklet and scalar identity";
                return false;
            }
            const tmcmt::TrackletId id = idNode.as<tmcmt::TrackletId>();
            identities.insert_or_assign(id, identityNode.Scalar());
        } catch (const std::exception& exception) {
            error = "ground truth line " + std::to_string(lineNumber) + ": " + exception.what();
            return false;
        }
    }
    return true;
}

std::string JsonEscape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

bool WriteAssignments(const std::string& path, const std::vector<tmcmt::Tracklet>& tracklets,
                      const tmcmt::flow::FlowAssociationResult& result, std::string& error) {
    std::ofstream output(path);
    if (!output) {
        error = "cannot open output '" + path + "'";
        return false;
    }
    for (const tmcmt::Tracklet& tracklet : tracklets) {
        const auto assignment = result.assignments.find(tracklet.id);
        const tmcmt::GlobalId globalId =
            assignment == result.assignments.end() ? tmcmt::kInvalidGlobalId : assignment->second;
        output << "{\"tracklet\":" << tracklet.id << ",\"global_id\":" << globalId
               << ",\"camera\":\"" << JsonEscape(tracklet.camera) << "\",\"begin_ms\":"
               << tracklet.begin.count() << ",\"end_ms\":" << tracklet.end.count() << "}\n";
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string error;
    const std::optional<Options> parsed = ParseArgs(argc, argv, error);
    if (!parsed) {
        std::cerr << "error: " << error << "\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_associate");
        return kExitUsageError;
    }
    const Options& options = *parsed;
    if (options.showHelp) {
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_associate");
        return kExitOk;
    }
    if (options.showVersion) {
        std::cout << tmcmt::kVersion << "\n";
        return kExitOk;
    }

    std::vector<tmcmt::Tracklet> tracklets;
    std::unordered_map<tmcmt::TrackletId, std::size_t> trackletIndex;
    if (!LoadTracklets(options.tracklets, options.format, tracklets, trackletIndex, error)) {
        std::cerr << "error: " << error << "\n";
        return kExitDataError;
    }

    std::optional<tmcmt::st::CameraLinkModel> cameraLinks;
    if (!options.cameraLinks.empty()) {
        auto loaded = tmcmt::st::CameraLinkModel::Load(options.cameraLinks);
        if (!loaded) {
            std::cerr << "error: " << loaded.status().ToString() << "\n";
            return kExitDataError;
        }
        cameraLinks = std::move(loaded.value());
    }
    const tmcmt::st::CameraLinkModel* cameraModel =
        cameraLinks ? &*cameraLinks : nullptr;

    int embeddingDim = 0;
    for (const tmcmt::Tracklet& tracklet : tracklets) {
        if (tracklet.embedding.IsValid()) {
            embeddingDim = tracklet.embedding.dim;
            break;
        }
    }

    std::vector<tmcmt::flow::TransitionEdge> transitions;
    if (embeddingDim > 0) {
        tmcmt::reid::GalleryOptions galleryOptions;
        galleryOptions.index.dim = embeddingDim;
        auto gallery = tmcmt::reid::GalleryManager::Create(tmcmt::IndexKind::Brute,
                                                           galleryOptions);
        if (!gallery) {
            std::cerr << "error: " << gallery.status().ToString() << "\n";
            return kExitDataError;
        }
        for (const tmcmt::Tracklet& tracklet : tracklets) {
            if (!tracklet.embedding.IsValid() || tracklet.embedding.dim != embeddingDim) {
                continue;
            }
            const tmcmt::Status added = gallery.value()->Add(tracklet);
            if (!added.ok()) {
                std::cerr << "warning: " << added.ToString() << "\n";
            }
        }

        tmcmt::st::PairGenerationOptions generationOptions;
        generationOptions.top_k = options.topK;
        generationOptions.min_similarity = options.minSimilarity;
        generationOptions.oversample = 4;
        generationOptions.exclude_same_camera = true;
        generationOptions.max_total_cost = options.maxTotalCost;
        generationOptions.weights = tmcmt::StWeights{1.0, 0.5, 0.3};
        generationOptions.window = cameraModel != nullptr
                                       ? cameraModel->Fallback()
                                       : tmcmt::st::TransitionWindow{0, 120000};
        generationOptions.gate_mode = tmcmt::GateMode::Off;

        tmcmt::st::PairCandidateGenerator generator(*gallery.value(), cameraModel,
                                                    generationOptions);
        for (const tmcmt::Tracklet& tracklet : tracklets) {
            if (!tracklet.embedding.IsValid() || tracklet.embedding.dim != embeddingDim) {
                continue;
            }
            auto candidates = generator.Generate(tracklet);
            if (!candidates) {
                std::cerr << "error: " << candidates.status().ToString() << "\n";
                return kExitDataError;
            }
            for (const tmcmt::st::PairCandidate& candidate : candidates.value()) {
                transitions.push_back(tmcmt::flow::TransitionEdge{
                    candidate.query_id, candidate.candidate_id,
                    static_cast<double>(candidate.costs.total)});
            }
        }
    }

    tmcmt::flow::FlowAssociationOptions associationOptions;
    associationOptions.solver = options.solver;
    associationOptions.link_reward = options.linkReward;
    associationOptions.max_candidates_per_node = options.maxCandidates;
    associationOptions.max_component_nodes = options.maxComponentNodes;
    associationOptions.commit_lag = tmcmt::Timestamp(options.commitLagMs);

    tmcmt::flow::FlowAssociator associator(associationOptions);
    auto associated = associator.Associate(tracklets, transitions);
    if (!associated) {
        std::cerr << "error: " << associated.status().ToString() << "\n";
        return kExitDataError;
    }
    const tmcmt::flow::FlowAssociationResult& result = associated.value();

    if (!WriteAssignments(options.out, tracklets, result, error)) {
        std::cerr << "error: " << error << "\n";
        return kExitDataError;
    }

    std::optional<tmcmt::metrics::MetricsReport> report;
    if (!options.gt.empty()) {
        std::unordered_map<tmcmt::TrackletId, std::string> groundTruth;
        if (!LoadGroundTruth(options.gt, groundTruth, error)) {
            std::cerr << "error: " << error << "\n";
            return kExitDataError;
        }
        std::vector<tmcmt::metrics::TrackIdentity> gtEntries;
        gtEntries.reserve(groundTruth.size());
        for (const auto& [id, identity] : groundTruth) {
            tmcmt::metrics::TrackIdentity entry;
            entry.tracklet_id = id;
            entry.identity = identity;
            const auto tracklet = trackletIndex.find(id);
            if (tracklet != trackletIndex.end()) {
                entry.begin = tracklets[tracklet->second].begin;
            }
            gtEntries.push_back(std::move(entry));
        }
        std::vector<tmcmt::metrics::PredictedIdentity> predictedEntries;
        predictedEntries.reserve(tracklets.size());
        for (const tmcmt::Tracklet& tracklet : tracklets) {
            const auto assignment = result.assignments.find(tracklet.id);
            if (assignment != result.assignments.end()) {
                predictedEntries.push_back(
                    tmcmt::metrics::PredictedIdentity{tracklet.id, assignment->second});
            }
        }
        report = tmcmt::metrics::MotMetrics::Compute(gtEntries, predictedEntries);
    }

    if (options.json) {
        std::cout << "{\"tracklets\":" << result.stats.tracklets
                  << ",\"transitions\":" << result.stats.transitions
                  << ",\"rejected_transitions\":" << result.stats.rejected_transitions
                  << ",\"components\":" << result.stats.components
                  << ",\"skipped_components\":" << result.stats.skipped_components
                  << ",\"paths\":" << result.stats.paths
                  << ",\"augmentations\":" << result.stats.augmentations
                  << ",\"optimal\":" << (result.optimal ? "true" : "false")
                  << ",\"total_cost\":" << result.total_cost;
        if (report) {
            std::cout << ",\"idf1\":" << report->idf1 << ",\"idp\":" << report->idp
                      << ",\"idr\":" << report->idr << ",\"idsw\":" << report->idsw
                      << ",\"idtp\":" << report->idtp << ",\"idfp\":" << report->idfp
                      << ",\"idfn\":" << report->idfn;
        }
        std::cout << "}\n";
    } else {
        std::cout << "tracklets: " << result.stats.tracklets << "\n"
                  << "transitions: " << result.stats.transitions
                  << " (rejected: " << result.stats.rejected_transitions << ")\n"
                  << "components: " << result.stats.components
                  << " (skipped: " << result.stats.skipped_components << ")\n"
                  << "paths: " << result.stats.paths << "\n"
                  << "solver: " << tmcmt::ToString(options.solver)
                  << (result.optimal ? " (optimal)" : " (heuristic)") << "\n"
                  << "total_cost: " << result.total_cost << "\n"
                  << "wrote: " << options.out << "\n";
        if (report) {
            std::cout << "IDF1: " << std::fixed << std::setprecision(4) << report->idf1
                      << "  IDP: " << report->idp << "  IDR: " << report->idr
                      << "  IDSW: " << report->idsw << "\n"
                      << "IDTP: " << report->idtp << "  IDFP: " << report->idfp
                      << "  IDFN: " << report->idfn << "\n";
        }
    }
    return kExitOk;
}
