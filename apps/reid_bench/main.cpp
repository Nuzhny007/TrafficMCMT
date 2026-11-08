#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Eigen/Core>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/reid/EmbeddingIndex.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitRuntimeError = 3;

struct Options {
    std::size_t vectors = 200000;
    int dim = 512;
    std::size_t queries = 200;
    std::size_t k = 10;
    std::size_t threads = 0;
    int nlist = 4096;
    int nprobe = 32;
    int hnswM = 32;
    std::uint32_t seed = 42;
    tmcmt::IndexKind index = tmcmt::IndexKind::Brute;
    bool json = false;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT ReID index benchmark " << tmcmt::kVersion << "\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  --vectors <n>    Index size (default: 200000)\n"
              << "  --dim <n>        Embedding dimension (default: 512)\n"
              << "  --queries <n>    Number of measured queries (default: 200)\n"
              << "  --k <n>          Neighbours per query (default: 10)\n"
              << "  --index <kind>   brute|faiss_flat|faiss_ivf_pq|faiss_hnsw (default: brute)\n"
              << "  --threads <n>    Brute-force worker threads, 0 = auto (default: 0)\n"
              << "  --nlist <n>      FAISS IVF lists (default: 4096)\n"
              << "  --nprobe <n>     FAISS IVF probes (default: 32)\n"
              << "  --hnsw-m <n>     FAISS HNSW connectivity (default: 32)\n"
              << "  --seed <n>       Deterministic generator seed (default: 42)\n"
              << "  --json           Print a JSON report\n"
              << "  --version        Print version and exit\n"
              << "  -h, --help       Print this help and exit\n";
}

std::optional<tmcmt::IndexKind> ParseIndexKind(std::string_view text) {
    if (text == "brute") {
        return tmcmt::IndexKind::Brute;
    }
    if (text == "faiss_flat") {
        return tmcmt::IndexKind::FaissFlat;
    }
    if (text == "faiss_ivf_pq") {
        return tmcmt::IndexKind::FaissIvfPq;
    }
    if (text == "faiss_hnsw") {
        return tmcmt::IndexKind::FaissHnsw;
    }
    return std::nullopt;
}

template <typename T>
bool ParseUnsigned(const std::string& text, T& out) {
    try {
        const unsigned long long value = std::stoull(text);
        out = static_cast<T>(value);
        return true;
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
        } else if (arg == "--json") {
            options.json = true;
        } else if (arg == "--vectors") {
            auto value = nextValue("--vectors");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.vectors)) {
                error = "invalid value for --vectors: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--dim") {
            auto value = nextValue("--dim");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.dim)) {
                error = "invalid value for --dim: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--queries") {
            auto value = nextValue("--queries");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.queries)) {
                error = "invalid value for --queries: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--k") {
            auto value = nextValue("--k");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.k)) {
                error = "invalid value for --k: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--threads") {
            auto value = nextValue("--threads");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.threads)) {
                error = "invalid value for --threads: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--nlist") {
            auto value = nextValue("--nlist");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.nlist)) {
                error = "invalid value for --nlist: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--nprobe") {
            auto value = nextValue("--nprobe");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.nprobe)) {
                error = "invalid value for --nprobe: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--hnsw-m") {
            auto value = nextValue("--hnsw-m");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.hnswM)) {
                error = "invalid value for --hnsw-m: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--seed") {
            auto value = nextValue("--seed");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.seed)) {
                error = "invalid value for --seed: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--index") {
            auto value = nextValue("--index");
            if (!value) {
                return std::nullopt;
            }
            const auto kind = ParseIndexKind(*value);
            if (!kind) {
                error = "unknown index '" + *value + "', expected brute|faiss_flat|faiss_ivf_pq|faiss_hnsw";
                return std::nullopt;
            }
            options.index = *kind;
        } else {
            error = "unknown argument '" + arg + "'";
            return std::nullopt;
        }
    }

    if (!options.showHelp && !options.showVersion) {
        if (options.vectors == 0) {
            error = "--vectors must be > 0";
            return std::nullopt;
        }
        if (options.dim <= 0) {
            error = "--dim must be > 0";
            return std::nullopt;
        }
        if (options.queries == 0) {
            error = "--queries must be > 0";
            return std::nullopt;
        }
        if (options.k == 0) {
            error = "--k must be > 0";
            return std::nullopt;
        }
    }
    return options;
}

std::vector<float> GenerateVectors(std::size_t count, int dim, std::uint32_t seed) {
    std::mt19937 engine(seed);
    std::normal_distribution<float> distribution(0.f, 1.f);
    std::vector<float> data(count * static_cast<std::size_t>(dim));
    for (std::size_t row = 0; row < count; ++row) {
        float* values = data.data() + row * static_cast<std::size_t>(dim);
        float squared = 0.f;
        for (int column = 0; column < dim; ++column) {
            values[column] = distribution(engine);
            squared += values[column] * values[column];
        }
        const float inverse = 1.f / std::sqrt(squared);
        for (int column = 0; column < dim; ++column) {
            values[column] *= inverse;
        }
    }
    return data;
}

double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double rank = percentile / 100.0 * static_cast<double>(values.size());
    std::size_t index = static_cast<std::size_t>(std::ceil(rank));
    if (index > 0) {
        --index;
    }
    index = std::min(index, values.size() - 1);
    return values[index];
}

std::vector<tmcmt::TrackletId> GroundTruth(
    const std::vector<float>& data, std::size_t count, int dim, std::span<const float> query,
    std::size_t k, Eigen::VectorXf& scores, std::vector<std::size_t>& order) {
    const Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
        matrix(data.data(), static_cast<Eigen::Index>(count), dim);
    const Eigen::Map<const Eigen::VectorXf> vector(query.data(), dim);
    scores.noalias() = matrix * vector;

    order.resize(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    const std::size_t take = std::min(k, count);
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(take), order.end(),
                      [&scores](std::size_t lhs, std::size_t rhs) {
                          return scores(static_cast<Eigen::Index>(lhs)) >
                                 scores(static_cast<Eigen::Index>(rhs));
                      });

    std::vector<tmcmt::TrackletId> ids;
    ids.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        ids.push_back(static_cast<tmcmt::TrackletId>(order[i] + 1));
    }
    return ids;
}

} // namespace

int main(int argc, char** argv) {
    std::string parseError;
    std::optional<Options> parsed = ParseArgs(argc, argv, parseError);
    if (!parsed) {
        std::cerr << "error: " << parseError << "\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_reid_bench");
        return kExitUsageError;
    }
    if (parsed->showHelp) {
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_reid_bench");
        return kExitOk;
    }
    if (parsed->showVersion) {
        std::cout << "TrafficMCMT " << tmcmt::kVersion << '\n';
        return kExitOk;
    }

    const Options& options = *parsed;
    std::cout << "reid_bench: generating " << options.vectors << " x " << options.dim
              << " vectors (seed " << options.seed << ")\n";

    const std::vector<float> data = GenerateVectors(options.vectors, options.dim, options.seed);

    tmcmt::reid::EmbeddingIndexOptions indexOptions;
    indexOptions.dim = options.dim;
    indexOptions.threads = options.threads;
    indexOptions.nlist = options.nlist;
    indexOptions.nprobe = options.nprobe;
    indexOptions.hnsw_m = options.hnswM;

    auto index = tmcmt::reid::CreateEmbeddingIndex(options.index, indexOptions);
    if (!index) {
        std::cerr << "error: " << index.status().ToString() << '\n';
        return kExitRuntimeError;
    }

    std::vector<tmcmt::reid::IndexEntry> entries;
    entries.reserve(options.vectors);
    for (std::size_t row = 0; row < options.vectors; ++row) {
        entries.push_back(tmcmt::reid::IndexEntry{
            static_cast<tmcmt::TrackletId>(row + 1),
            std::span<const float>(data.data() + row * static_cast<std::size_t>(options.dim),
                                   static_cast<std::size_t>(options.dim))});
    }

    const auto buildStart = std::chrono::steady_clock::now();
    const auto rebuild = index.value()->Rebuild(entries);
    const auto buildStop = std::chrono::steady_clock::now();
    if (!rebuild) {
        std::cerr << "error: " << rebuild.ToString() << '\n';
        return kExitRuntimeError;
    }
    const double buildMs =
        std::chrono::duration<double, std::milli>(buildStop - buildStart).count();

    const std::vector<float> queryData = GenerateVectors(options.queries, options.dim, options.seed + 1);
    std::vector<double> latenciesMs;
    latenciesMs.reserve(options.queries);

    const auto measureStart = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < options.queries; ++i) {
        const std::span<const float> query(
            queryData.data() + i * static_cast<std::size_t>(options.dim),
            static_cast<std::size_t>(options.dim));
        const auto start = std::chrono::steady_clock::now();
        const auto hits = index.value()->Search(query, options.k);
        const auto stop = std::chrono::steady_clock::now();
        if (!hits) {
            std::cerr << "error: " << hits.status().ToString() << '\n';
            return kExitRuntimeError;
        }
        latenciesMs.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    const auto measureStop = std::chrono::steady_clock::now();
    const double totalSeconds = std::chrono::duration<double>(measureStop - measureStart).count();

    double recall = 1.0;
    std::size_t recallQueries = 0;
    if (options.index != tmcmt::IndexKind::Brute) {
        recallQueries = std::min<std::size_t>(options.queries, 50);
        Eigen::VectorXf scores(static_cast<Eigen::Index>(options.vectors));
        std::vector<std::size_t> order;
        std::size_t matched = 0;
        std::size_t total = 0;
        for (std::size_t i = 0; i < recallQueries; ++i) {
            const std::span<const float> query(
                queryData.data() + i * static_cast<std::size_t>(options.dim),
                static_cast<std::size_t>(options.dim));
            const auto reference =
                GroundTruth(data, options.vectors, options.dim, query, options.k, scores, order);
            const std::set<tmcmt::TrackletId> expected(reference.begin(), reference.end());
            const auto hits = index.value()->Search(query, options.k);
            if (!hits) {
                std::cerr << "error: " << hits.status().ToString() << '\n';
                return kExitRuntimeError;
            }
            for (const auto& hit : hits.value()) {
                if (expected.count(hit.id) != 0) {
                    ++matched;
                }
            }
            total += reference.size();
        }
        recall = total == 0 ? 0.0 : static_cast<double>(matched) / static_cast<double>(total);
    }

    const double p50 = Percentile(latenciesMs, 50.0);
    const double p95 = Percentile(latenciesMs, 95.0);
    const double p99 = Percentile(latenciesMs, 99.0);
    const double maxLatency = Percentile(latenciesMs, 100.0);
    const double qps = totalSeconds > 0.0 ? static_cast<double>(options.queries) / totalSeconds : 0.0;

    const char* kind = tmcmt::ToString(options.index);
    if (options.json) {
        std::cout << std::fixed << std::setprecision(3) << "{"
                  << "\"index\":\"" << kind << "\","
                  << "\"vectors\":" << options.vectors << ","
                  << "\"dim\":" << options.dim << ","
                  << "\"queries\":" << options.queries << ","
                  << "\"k\":" << options.k << ","
                  << "\"threads\":" << options.threads << ","
                  << "\"faiss_available\":" << (tmcmt::reid::IsFaissAvailable() ? "true" : "false") << ","
                  << "\"build_ms\":" << buildMs << ","
                  << "\"p50_ms\":" << p50 << ","
                  << "\"p95_ms\":" << p95 << ","
                  << "\"p99_ms\":" << p99 << ","
                  << "\"max_ms\":" << maxLatency << ","
                  << "\"qps\":" << qps << ","
                  << "\"recall_at_k\":" << recall << ","
                  << "\"recall_queries\":" << recallQueries << "}\n";
    } else {
        std::cout << std::fixed << std::setprecision(3)
                  << "index: " << kind << (tmcmt::reid::IsFaissAvailable() ? "" : " (FAISS not built)") << '\n'
                  << "vectors: " << options.vectors << '\n'
                  << "dim: " << options.dim << '\n'
                  << "queries: " << options.queries << " (k=" << options.k << ")\n"
                  << "build_ms: " << buildMs << '\n'
                  << "latency_ms: p50=" << p50 << " p95=" << p95 << " p99=" << p99
                  << " max=" << maxLatency << '\n'
                  << "qps: " << qps << '\n'
                  << "recall@" << options.k << ": " << recall;
        if (recallQueries != 0) {
            std::cout << " (measured on " << recallQueries << " queries)";
        }
        std::cout << '\n';
    }
    return kExitOk;
}
