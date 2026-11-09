#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt {

// ---------------------------------------------------------------------------
// Enumerations (string values are part of the configuration contract)
// ---------------------------------------------------------------------------

enum class SourceKind : std::uint8_t {
    File = 0,
    Zmq = 1,
};

enum class IndexKind : std::uint8_t {
    Brute = 0,
    FaissFlat = 1,
    FaissIvfPq = 2,
    FaissHnsw = 3,
};

enum class GateMode : std::uint8_t {
    Off = 0,
    Zone = 1,
    Bev = 2,
};

enum class SolverKind : std::uint8_t {
    MuSsp = 0,
    Ssp = 1,
    Greedy = 2,
};

/// On-disk tracklet format. `Auto` sniffs the extension and then the first
/// non-whitespace byte of the file.
enum class TrackletFileFormat : std::uint8_t {
    Auto = 0,
    Jsonl = 1,
    Protobuf = 2,
};

const char* ToString(SourceKind kind) noexcept;
const char* ToString(IndexKind kind) noexcept;
const char* ToString(GateMode mode) noexcept;
const char* ToString(SolverKind kind) noexcept;
const char* ToString(TrackletFileFormat format) noexcept;

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

struct LogConfig {
    std::string level = "info";
    std::string file = "logs/trafficmcmt.log";
    bool console = true;
    std::size_t max_size_bytes = 10u * 1024u * 1024u;
    std::size_t max_files = 5;
};

struct ZmqSourceConfig {
    std::string endpoint = "tcp://0.0.0.0:5556";
    std::string subscribe_prefix = "tmcmt.v1.";
    int rcv_hwm = 5000;
    int reconnect_ivl_ms = 1000;
};

struct FileSourceConfig {
    std::string path = "data/tracklets.jsonl";
    /// Replay speed multiplier. 0 disables pacing (as fast as possible).
    double speed = 1.0;
    TrackletFileFormat format = TrackletFileFormat::Auto;
};

struct IngestConfig {
    SourceKind source = SourceKind::File;
    int max_lateness_ms = 2000;
    /// Reject protocol violations instead of repairing them.
    bool strict = true;
    FileSourceConfig file;
    ZmqSourceConfig zmq;
};

struct ReIdConfig {
    int embedding_dim = 512;
    IndexKind index = IndexKind::Brute;
    int nlist = 4096;
    int nprobe = 32;
    int top_k = 20;
    /// Drop appearance candidates below this cosine similarity.
    double min_similarity = 0.0;
    /// Do not match a tracklet against tracklets from its own camera.
    bool exclude_same_camera = true;
    std::string whitening;
    /// Store embeddings as int8 in the binary protocol (lossy, ~4x smaller).
    bool quantize_embeddings = false;
};

struct StWeights {
    double appearance = 1.0;
    double temporal = 0.5;
    double spatial = 0.3;
};

struct GateConfig {
    GateMode mode = GateMode::Off;
    double max_speed_mps = 30.0;
    double max_distance_m = 200.0;
};

struct StConfig {
    int min_transition_ms = 0;
    int max_transition_ms = 120000;
    StWeights weights;
    GateConfig gate;
    /// Camera topology and transition statistics (YAML). Empty disables the
    /// link and measured-transition checks.
    std::string camera_links;
};

struct FlowConfig {
    SolverKind solver = SolverKind::Ssp;
    int max_candidates_per_node = 15;
    int max_component_nodes = 20000;
};

struct StorageConfig {
    std::string backend = "memory";
    std::string path = "data/trafficmcmt.db";
    int retention_days = 7;
};

struct JsonlSinkConfig {
    std::string path = "data/global_tracks.jsonl";
};

struct BinarySinkConfig {
    std::string path = "data/global_tracks.bin";
    bool quantize_embeddings = false;
};

struct ZmqSinkConfig {
    std::string endpoint = "tcp://0.0.0.0:5557";
};

struct OutputConfig {
    std::vector<std::string> sinks{"jsonl"};
    JsonlSinkConfig jsonl;
    BinarySinkConfig binary;
    ZmqSinkConfig zmq;
};

struct MetricsConfig {
    bool enabled = false;
    int http_port = 9100;
};

// ---------------------------------------------------------------------------
// Root configuration
// ---------------------------------------------------------------------------

struct MasterConfig {
    PipelineMode mode = PipelineMode::Offline;
    int window_ms = 30000;
    int window_overlap_ms = 10000;
    int commit_lag_ms = 15000;

    IngestConfig ingest;
    ReIdConfig reid;
    StConfig st;
    FlowConfig flow;
    StorageConfig storage;
    OutputConfig output;
    MetricsConfig metrics;
    LogConfig log;

    /// Loads and validates the configuration. Never throws.
    [[nodiscard]] static Result<MasterConfig> Load(const std::string& path);

    /// Cross-section validation. Returns the first violation with its key path.
    [[nodiscard]] Status Validate() const;
};

/// Loads a standalone logging configuration (flat mapping or a `log:` section).
[[nodiscard]] Result<LogConfig> LoadLogConfig(const std::string& path);

/// Serializes the effective configuration back to YAML (used by --dump-config).
[[nodiscard]] std::string Dump(const MasterConfig& config);

} // namespace tmcmt
