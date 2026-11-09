#include "trafficmcmt/core/Config.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace tmcmt {
namespace {

using detail::EqualsIgnoreCase;

std::string JoinKey(const std::string& prefix, const char* key) {
    if (prefix.empty()) {
        return std::string(key);
    }
    return prefix + "." + key;
}

Status MakeError(ErrorCode code, const std::string& where, const std::string& what) {
    return Status::Error(code, where + ": " + what);
}

/// yaml-cpp usually reports the position itself; add it only when it is missing.
std::string WithMark(const YAML::Exception& error, const YAML::Node& node) {
    std::string message = error.what();
    if (message.find("line ") == std::string::npos) {
        message += " (line " + std::to_string(node.Mark().line + 1) + ")";
    }
    return message;
}

Status ExpectMap(const YAML::Node& node, const std::string& where, YAML::Node& out) {
    if (!node || node.IsNull()) {
        out = YAML::Node(YAML::NodeType::Map);
        return Status::Ok();
    }
    if (!node.IsMap()) {
        return MakeError(ErrorCode::ParseError, where, "expected a mapping");
    }
    out = node;
    return Status::Ok();
}

template <typename T>
Status ReadScalar(const YAML::Node& parent, const char* key, T& out, const std::string& where) {
    if (!parent || !parent.IsMap()) {
        return Status::Ok();
    }
    const YAML::Node node = parent[key];
    if (!node || node.IsNull()) {
        return Status::Ok();
    }
    try {
        out = node.as<T>();
    } catch (const YAML::Exception& e) {
        return MakeError(ErrorCode::ParseError, JoinKey(where, key), WithMark(e, node));
    }
    return Status::Ok();
}

Status ReadSection(const YAML::Node& root, const char* key, YAML::Node& out, const std::string& where) {
    if (!root || !root.IsMap()) {
        out = YAML::Node(YAML::NodeType::Map);
        return Status::Ok();
    }
    return ExpectMap(root[key], JoinKey(where, key), out);
}

Status ReadStringList(const YAML::Node& parent, const char* key, std::vector<std::string>& out,
                      const std::string& where) {
    if (!parent || !parent.IsMap()) {
        return Status::Ok();
    }
    const YAML::Node node = parent[key];
    if (!node || node.IsNull()) {
        return Status::Ok();
    }
    if (!node.IsSequence()) {
        return MakeError(ErrorCode::ParseError, JoinKey(where, key), "expected a list");
    }
    try {
        out = node.as<std::vector<std::string>>();
    } catch (const YAML::Exception& e) {
        return MakeError(ErrorCode::ParseError, JoinKey(where, key), WithMark(e, node));
    }
    return Status::Ok();
}

std::optional<SourceKind> ParseSourceKind(std::string_view text) {
    if (EqualsIgnoreCase(text, "file")) {
        return SourceKind::File;
    }
    if (EqualsIgnoreCase(text, "zmq")) {
        return SourceKind::Zmq;
    }
    return std::nullopt;
}

std::optional<IndexKind> ParseIndexKind(std::string_view text) {
    if (EqualsIgnoreCase(text, "brute")) {
        return IndexKind::Brute;
    }
    if (EqualsIgnoreCase(text, "faiss_flat")) {
        return IndexKind::FaissFlat;
    }
    if (EqualsIgnoreCase(text, "faiss_ivf_pq")) {
        return IndexKind::FaissIvfPq;
    }
    if (EqualsIgnoreCase(text, "faiss_hnsw")) {
        return IndexKind::FaissHnsw;
    }
    return std::nullopt;
}

std::optional<GateMode> ParseGateMode(std::string_view text) {
    if (EqualsIgnoreCase(text, "off")) {
        return GateMode::Off;
    }
    if (EqualsIgnoreCase(text, "zone")) {
        return GateMode::Zone;
    }
    if (EqualsIgnoreCase(text, "bev")) {
        return GateMode::Bev;
    }
    return std::nullopt;
}

std::optional<TrackletFileFormat> ParseTrackletFileFormat(std::string_view text) {
    if (EqualsIgnoreCase(text, "auto")) {
        return TrackletFileFormat::Auto;
    }
    if (EqualsIgnoreCase(text, "jsonl") || EqualsIgnoreCase(text, "json")) {
        return TrackletFileFormat::Jsonl;
    }
    if (EqualsIgnoreCase(text, "protobuf") || EqualsIgnoreCase(text, "pb") || EqualsIgnoreCase(text, "bin")) {
        return TrackletFileFormat::Protobuf;
    }
    return std::nullopt;
}

std::optional<SolverKind> ParseSolverKind(std::string_view text) {
    if (EqualsIgnoreCase(text, "mussp")) {
        return SolverKind::MuSsp;
    }
    if (EqualsIgnoreCase(text, "ssp")) {
        return SolverKind::Ssp;
    }
    if (EqualsIgnoreCase(text, "greedy")) {
        return SolverKind::Greedy;
    }
    return std::nullopt;
}

template <typename EnumT, typename ParserT>
Status ReadEnum(const YAML::Node& parent, const char* key, EnumT& out, ParserT parser, const std::string& where,
                const char* allowed) {
    std::string raw;
    Status status = ReadScalar(parent, key, raw, where);
    if (!status.ok()) {
        return status;
    }
    if (raw.empty()) {
        return Status::Ok();
    }
    const auto parsed = parser(std::string_view(raw));
    if (!parsed) {
        return MakeError(ErrorCode::ParseError, JoinKey(where, key),
                         "unknown value '" + raw + "', expected one of: " + allowed);
    }
    out = *parsed;
    return Status::Ok();
}

bool IsKnownLogLevel(const std::string& level) {
    static constexpr const char* kLevels[] = {"trace", "debug", "info", "warn", "error", "critical", "off"};
    return std::any_of(std::begin(kLevels), std::end(kLevels),
                       [&level](const char* known) { return EqualsIgnoreCase(level, known); });
}

bool IsKnownStorageBackend(const std::string& backend) {
    return EqualsIgnoreCase(backend, "memory") || EqualsIgnoreCase(backend, "sqlite");
}

bool IsKnownSink(const std::string& sink) {
    return EqualsIgnoreCase(sink, "jsonl") || EqualsIgnoreCase(sink, "binary") || EqualsIgnoreCase(sink, "csv") ||
           EqualsIgnoreCase(sink, "zmq");
}

// ---------------------------------------------------------------------------
// Section parsers
// ---------------------------------------------------------------------------

Status ParseLogNode(const YAML::Node& node, LogConfig& out) {
    const std::string where = "log";
    Status status = ReadScalar(node, "level", out.level, where);
    if (!status.ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "file", out.file, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "console", out.console, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "max_size_bytes", out.max_size_bytes, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "max_files", out.max_files, where)).ok()) {
        return status;
    }
    return Status::Ok();
}

Status ParseIngest(const YAML::Node& root, IngestConfig& out) {
    YAML::Node node;
    Status status = ReadSection(root, "ingest", node, "");
    if (!status.ok()) {
        return status;
    }

    if (!(status = ReadEnum(node, "source", out.source, ParseSourceKind, "ingest", "file|zmq")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "max_lateness_ms", out.max_lateness_ms, "ingest")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "strict", out.strict, "ingest")).ok()) {
        return status;
    }

    YAML::Node file;
    if (!(status = ReadSection(node, "file", file, "ingest")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(file, "path", out.file.path, "ingest.file")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(file, "speed", out.file.speed, "ingest.file")).ok()) {
        return status;
    }
    if (!(status = ReadEnum(file, "format", out.file.format, ParseTrackletFileFormat, "ingest.file",
                            "auto|jsonl|protobuf"))
             .ok()) {
        return status;
    }

    YAML::Node zmq;
    if (!(status = ReadSection(node, "zmq", zmq, "ingest")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(zmq, "endpoint", out.zmq.endpoint, "ingest.zmq")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(zmq, "subscribe_prefix", out.zmq.subscribe_prefix, "ingest.zmq")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(zmq, "rcv_hwm", out.zmq.rcv_hwm, "ingest.zmq")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(zmq, "reconnect_ivl_ms", out.zmq.reconnect_ivl_ms, "ingest.zmq")).ok()) {
        return status;
    }
    return Status::Ok();
}

Status ParseReId(const YAML::Node& root, ReIdConfig& out) {
    YAML::Node node;
    Status status = ReadSection(root, "reid", node, "");
    if (!status.ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "embedding_dim", out.embedding_dim, "reid")).ok()) {
        return status;
    }
    if (!(status = ReadEnum(node, "index", out.index, ParseIndexKind, "reid",
                            "brute|faiss_flat|faiss_ivf_pq|faiss_hnsw"))
             .ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "nlist", out.nlist, "reid")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "nprobe", out.nprobe, "reid")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "top_k", out.top_k, "reid")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "min_similarity", out.min_similarity, "reid")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "exclude_same_camera", out.exclude_same_camera, "reid")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "whitening", out.whitening, "reid")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "quantize_embeddings", out.quantize_embeddings, "reid")).ok()) {
        return status;
    }
    return Status::Ok();
}

Status ParseSt(const YAML::Node& root, StConfig& out) {
    YAML::Node node;
    Status status = ReadSection(root, "st", node, "");
    if (!status.ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "min_transition_ms", out.min_transition_ms, "st")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "max_transition_ms", out.max_transition_ms, "st")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "camera_links", out.camera_links, "st")).ok()) {
        return status;
    }

    YAML::Node weights;
    if (!(status = ReadSection(node, "weights", weights, "st")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(weights, "appearance", out.weights.appearance, "st.weights")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(weights, "temporal", out.weights.temporal, "st.weights")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(weights, "spatial", out.weights.spatial, "st.weights")).ok()) {
        return status;
    }

    YAML::Node gate;
    if (!(status = ReadSection(node, "gate", gate, "st")).ok()) {
        return status;
    }
    if (!(status = ReadEnum(gate, "mode", out.gate.mode, ParseGateMode, "st.gate", "off|zone|bev")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(gate, "max_speed_mps", out.gate.max_speed_mps, "st.gate")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(gate, "max_distance_m", out.gate.max_distance_m, "st.gate")).ok()) {
        return status;
    }
    return Status::Ok();
}

Status ParseFlow(const YAML::Node& root, FlowConfig& out) {
    YAML::Node node;
    Status status = ReadSection(root, "flow", node, "");
    if (!status.ok()) {
        return status;
    }
    if (!(status = ReadEnum(node, "solver", out.solver, ParseSolverKind, "flow", "mussp|ssp|greedy")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "max_candidates_per_node", out.max_candidates_per_node, "flow")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "link_reward", out.link_reward, "flow")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "max_component_nodes", out.max_component_nodes, "flow")).ok()) {
        return status;
    }
    return Status::Ok();
}

Status ParseStorage(const YAML::Node& root, StorageConfig& out) {
    YAML::Node node;
    Status status = ReadSection(root, "storage", node, "");
    if (!status.ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "backend", out.backend, "storage")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "path", out.path, "storage")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "retention_days", out.retention_days, "storage")).ok()) {
        return status;
    }
    return Status::Ok();
}

Status ParseOutput(const YAML::Node& root, OutputConfig& out) {
    YAML::Node node;
    Status status = ReadSection(root, "output", node, "");
    if (!status.ok()) {
        return status;
    }
    if (!(status = ReadStringList(node, "sinks", out.sinks, "output")).ok()) {
        return status;
    }

    YAML::Node jsonl;
    if (!(status = ReadSection(node, "jsonl", jsonl, "output")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(jsonl, "path", out.jsonl.path, "output.jsonl")).ok()) {
        return status;
    }

    YAML::Node binary;
    if (!(status = ReadSection(node, "binary", binary, "output")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(binary, "path", out.binary.path, "output.binary")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(binary, "quantize_embeddings", out.binary.quantize_embeddings, "output.binary")).ok()) {
        return status;
    }

    YAML::Node zmq;
    if (!(status = ReadSection(node, "zmq", zmq, "output")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(zmq, "endpoint", out.zmq.endpoint, "output.zmq")).ok()) {
        return status;
    }
    return Status::Ok();
}

Status ParseMetrics(const YAML::Node& root, MetricsConfig& out) {
    YAML::Node node;
    Status status = ReadSection(root, "metrics", node, "");
    if (!status.ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "enabled", out.enabled, "metrics")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "http_port", out.http_port, "metrics")).ok()) {
        return status;
    }
    return Status::Ok();
}

Status ParseMasterSection(const YAML::Node& root, MasterConfig& out) {
    YAML::Node node;
    Status status = ReadSection(root, "master", node, "");
    if (!status.ok()) {
        return status;
    }
    std::string mode;
    if (!(status = ReadScalar(node, "mode", mode, "master")).ok()) {
        return status;
    }
    if (!mode.empty()) {
        const auto parsed = ParsePipelineMode(std::string_view(mode));
        if (!parsed) {
            return MakeError(ErrorCode::ParseError, "master.mode",
                             "unknown value '" + mode + "', expected one of: offline|online");
        }
        out.mode = *parsed;
    }
    if (!(status = ReadScalar(node, "window_ms", out.window_ms, "master")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "window_overlap_ms", out.window_overlap_ms, "master")).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "commit_lag_ms", out.commit_lag_ms, "master")).ok()) {
        return status;
    }
    return Status::Ok();
}

Status LoadYaml(const std::string& path, YAML::Node& root) {
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::BadFile&) {
        return Status::Error(ErrorCode::IoError, "cannot open configuration file: " + path);
    } catch (const YAML::Exception& e) {
        return Status::Error(ErrorCode::ParseError, path + ": " + e.what());
    } catch (const std::exception& e) {
        return Status::Error(ErrorCode::Internal, path + ": " + e.what());
    }

    if (!root || root.IsNull()) {
        return Status::Error(ErrorCode::ParseError, path + ": configuration is empty");
    }
    if (!root.IsMap()) {
        return Status::Error(ErrorCode::ParseError, path + ": root element must be a mapping");
    }
    return Status::Ok();
}

} // namespace

// ---------------------------------------------------------------------------
// Enum stringification
// ---------------------------------------------------------------------------

const char* ToString(SourceKind kind) noexcept {
    switch (kind) {
    case SourceKind::File:
        return "file";
    case SourceKind::Zmq:
        return "zmq";
    }
    return "file";
}

const char* ToString(IndexKind kind) noexcept {
    switch (kind) {
    case IndexKind::Brute:
        return "brute";
    case IndexKind::FaissFlat:
        return "faiss_flat";
    case IndexKind::FaissIvfPq:
        return "faiss_ivf_pq";
    case IndexKind::FaissHnsw:
        return "faiss_hnsw";
    }
    return "brute";
}

const char* ToString(GateMode mode) noexcept {
    switch (mode) {
    case GateMode::Off:
        return "off";
    case GateMode::Zone:
        return "zone";
    case GateMode::Bev:
        return "bev";
    }
    return "off";
}

const char* ToString(SolverKind kind) noexcept {
    switch (kind) {
    case SolverKind::MuSsp:
        return "mussp";
    case SolverKind::Ssp:
        return "ssp";
    case SolverKind::Greedy:
        return "greedy";
    }
    return "ssp";
}

const char* ToString(TrackletFileFormat format) noexcept {
    switch (format) {
    case TrackletFileFormat::Auto:
        return "auto";
    case TrackletFileFormat::Jsonl:
        return "jsonl";
    case TrackletFileFormat::Protobuf:
        return "protobuf";
    }
    return "auto";
}

// ---------------------------------------------------------------------------
// MasterConfig
// ---------------------------------------------------------------------------

Result<MasterConfig> MasterConfig::Load(const std::string& path) {
    YAML::Node root;
    Status status = LoadYaml(path, root);
    if (!status.ok()) {
        return status;
    }

    MasterConfig config;
    status = ParseMasterSection(root, config);
    if (status.ok()) {
        status = ParseIngest(root, config.ingest);
    }
    if (status.ok()) {
        status = ParseReId(root, config.reid);
    }
    if (status.ok()) {
        status = ParseSt(root, config.st);
    }
    if (status.ok()) {
        status = ParseFlow(root, config.flow);
    }
    if (status.ok()) {
        status = ParseStorage(root, config.storage);
    }
    if (status.ok()) {
        status = ParseOutput(root, config.output);
    }
    if (status.ok()) {
        status = ParseMetrics(root, config.metrics);
    }
    if (status.ok()) {
        YAML::Node logNode;
        status = ReadSection(root, "log", logNode, "");
        if (status.ok()) {
            status = ParseLogNode(logNode, config.log);
        }
    }
    if (!status.ok()) {
        return status;
    }

    status = config.Validate();
    if (!status.ok()) {
        return status;
    }
    return config;
}

Status MasterConfig::Validate() const {
    const auto fail = [](const char* key, const std::string& what) {
        return Status::Error(ErrorCode::InvalidArgument, std::string(key) + ": " + what);
    };

    if (window_ms <= 0) {
        return fail("master.window_ms", "must be > 0");
    }
    if (window_overlap_ms < 0 || window_overlap_ms >= window_ms) {
        return fail("master.window_overlap_ms", "must be >= 0 and < master.window_ms");
    }
    if (commit_lag_ms < 0) {
        return fail("master.commit_lag_ms", "must be >= 0");
    }

    if (ingest.max_lateness_ms < 0) {
        return fail("ingest.max_lateness_ms", "must be >= 0");
    }
    if (ingest.source == SourceKind::File) {
        if (ingest.file.path.empty()) {
            return fail("ingest.file.path", "must not be empty for source=file");
        }
        if (!(ingest.file.speed > 0.0)) {
            return fail("ingest.file.speed", "must be > 0");
        }
    } else {
        if (ingest.zmq.endpoint.empty()) {
            return fail("ingest.zmq.endpoint", "must not be empty for source=zmq");
        }
        if (ingest.zmq.subscribe_prefix.empty()) {
            return fail("ingest.zmq.subscribe_prefix", "must not be empty");
        }
        if (ingest.zmq.rcv_hwm <= 0) {
            return fail("ingest.zmq.rcv_hwm", "must be > 0");
        }
        if (ingest.zmq.reconnect_ivl_ms < 0) {
            return fail("ingest.zmq.reconnect_ivl_ms", "must be >= 0");
        }
    }

    if (reid.embedding_dim <= 0) {
        return fail("reid.embedding_dim", "must be > 0");
    }
    if (reid.top_k <= 0) {
        return fail("reid.top_k", "must be > 0");
    }
    if (reid.nlist <= 0) {
        return fail("reid.nlist", "must be > 0");
    }
    if (reid.nprobe <= 0 || reid.nprobe > reid.nlist) {
        return fail("reid.nprobe", "must be > 0 and <= reid.nlist");
    }
    if (reid.min_similarity < -1.0 || reid.min_similarity > 1.0) {
        return fail("reid.min_similarity", "must be in [-1, 1]");
    }

    if (st.min_transition_ms < 0) {
        return fail("st.min_transition_ms", "must be >= 0");
    }
    if (st.max_transition_ms <= st.min_transition_ms) {
        return fail("st.max_transition_ms", "must be > st.min_transition_ms");
    }
    const double weightSum = st.weights.appearance + st.weights.temporal + st.weights.spatial;
    if (st.weights.appearance < 0.0 || st.weights.temporal < 0.0 || st.weights.spatial < 0.0) {
        return fail("st.weights", "weights must be >= 0");
    }
    if (!(weightSum > 0.0)) {
        return fail("st.weights", "at least one weight must be > 0");
    }
    if (st.gate.mode != GateMode::Off) {
        if (!(st.gate.max_speed_mps > 0.0)) {
            return fail("st.gate.max_speed_mps", "must be > 0");
        }
        if (!(st.gate.max_distance_m > 0.0)) {
            return fail("st.gate.max_distance_m", "must be > 0");
        }
    }

    if (flow.max_candidates_per_node <= 0) {
        return fail("flow.max_candidates_per_node", "must be > 0");
    }
    if (!(flow.link_reward > 0.0) || !std::isfinite(flow.link_reward)) {
        return fail("flow.link_reward", "must be a finite value > 0");
    }
    if (flow.max_component_nodes <= 0) {
        return fail("flow.max_component_nodes", "must be > 0");
    }

    if (!IsKnownStorageBackend(storage.backend)) {
        return fail("storage.backend", "expected one of: memory|sqlite");
    }
    if (EqualsIgnoreCase(storage.backend, "sqlite") && storage.path.empty()) {
        return fail("storage.path", "must not be empty for backend=sqlite");
    }
    if (storage.retention_days <= 0) {
        return fail("storage.retention_days", "must be > 0");
    }

    if (output.sinks.empty()) {
        return fail("output.sinks", "at least one sink is required");
    }
    for (const std::string& sink : output.sinks) {
        if (!IsKnownSink(sink)) {
            return fail("output.sinks", "unknown sink '" + sink + "', expected one of: jsonl|binary|csv|zmq");
        }
        if (EqualsIgnoreCase(sink, "jsonl") && output.jsonl.path.empty()) {
            return fail("output.jsonl.path", "must not be empty when the jsonl sink is enabled");
        }
        if (EqualsIgnoreCase(sink, "binary") && output.binary.path.empty()) {
            return fail("output.binary.path", "must not be empty when the binary sink is enabled");
        }
        if (EqualsIgnoreCase(sink, "zmq") && output.zmq.endpoint.empty()) {
            return fail("output.zmq.endpoint", "must not be empty when the zmq sink is enabled");
        }
    }

    if (metrics.enabled && (metrics.http_port < 1 || metrics.http_port > 65535)) {
        return fail("metrics.http_port", "must be in [1, 65535]");
    }

    if (!IsKnownLogLevel(log.level)) {
        return fail("log.level", "unknown level '" + log.level + "'");
    }
    if (log.max_size_bytes == 0) {
        return fail("log.max_size_bytes", "must be > 0");
    }
    if (log.max_files == 0) {
        return fail("log.max_files", "must be > 0");
    }

    return Status::Ok();
}

Result<LogConfig> LoadLogConfig(const std::string& path) {
    YAML::Node root;
    Status status = LoadYaml(path, root);
    if (!status.ok()) {
        return status;
    }

    YAML::Node node = root;
    if (root["log"] && !root["log"].IsNull()) {
        if (!root["log"].IsMap()) {
            return Status::Error(ErrorCode::ParseError, "log: expected a mapping");
        }
        node = root["log"];
    }

    LogConfig config;
    status = ParseLogNode(node, config);
    if (!status.ok()) {
        return status;
    }
    if (!IsKnownLogLevel(config.level)) {
        return Status::Error(ErrorCode::InvalidArgument, "log.level: unknown level '" + config.level + "'");
    }
    if (config.max_size_bytes == 0) {
        return Status::Error(ErrorCode::InvalidArgument, "log.max_size_bytes: must be > 0");
    }
    if (config.max_files == 0) {
        return Status::Error(ErrorCode::InvalidArgument, "log.max_files: must be > 0");
    }
    return config;
}

std::string Dump(const MasterConfig& config) {
    YAML::Node root;

    root["master"]["mode"] = ToString(config.mode);
    root["master"]["window_ms"] = config.window_ms;
    root["master"]["window_overlap_ms"] = config.window_overlap_ms;
    root["master"]["commit_lag_ms"] = config.commit_lag_ms;

    root["ingest"]["source"] = ToString(config.ingest.source);
    root["ingest"]["max_lateness_ms"] = config.ingest.max_lateness_ms;
    root["ingest"]["strict"] = config.ingest.strict;
    root["ingest"]["file"]["path"] = config.ingest.file.path;
    root["ingest"]["file"]["speed"] = config.ingest.file.speed;
    root["ingest"]["file"]["format"] = ToString(config.ingest.file.format);
    root["ingest"]["zmq"]["endpoint"] = config.ingest.zmq.endpoint;
    root["ingest"]["zmq"]["subscribe_prefix"] = config.ingest.zmq.subscribe_prefix;
    root["ingest"]["zmq"]["rcv_hwm"] = config.ingest.zmq.rcv_hwm;
    root["ingest"]["zmq"]["reconnect_ivl_ms"] = config.ingest.zmq.reconnect_ivl_ms;

    root["reid"]["embedding_dim"] = config.reid.embedding_dim;
    root["reid"]["index"] = ToString(config.reid.index);
    root["reid"]["nlist"] = config.reid.nlist;
    root["reid"]["nprobe"] = config.reid.nprobe;
    root["reid"]["top_k"] = config.reid.top_k;
    root["reid"]["min_similarity"] = config.reid.min_similarity;
    root["reid"]["exclude_same_camera"] = config.reid.exclude_same_camera;
    root["reid"]["whitening"] = config.reid.whitening;
    root["reid"]["quantize_embeddings"] = config.reid.quantize_embeddings;

    root["st"]["min_transition_ms"] = config.st.min_transition_ms;
    root["st"]["max_transition_ms"] = config.st.max_transition_ms;
    root["st"]["camera_links"] = config.st.camera_links;
    root["st"]["weights"]["appearance"] = config.st.weights.appearance;
    root["st"]["weights"]["temporal"] = config.st.weights.temporal;
    root["st"]["weights"]["spatial"] = config.st.weights.spatial;
    root["st"]["gate"]["mode"] = ToString(config.st.gate.mode);
    root["st"]["gate"]["max_speed_mps"] = config.st.gate.max_speed_mps;
    root["st"]["gate"]["max_distance_m"] = config.st.gate.max_distance_m;

    root["flow"]["solver"] = ToString(config.flow.solver);
    root["flow"]["link_reward"] = config.flow.link_reward;
    root["flow"]["max_candidates_per_node"] = config.flow.max_candidates_per_node;
    root["flow"]["max_component_nodes"] = config.flow.max_component_nodes;

    root["storage"]["backend"] = config.storage.backend;
    root["storage"]["path"] = config.storage.path;
    root["storage"]["retention_days"] = config.storage.retention_days;

    root["output"]["sinks"] = config.output.sinks;
    root["output"]["jsonl"]["path"] = config.output.jsonl.path;
    root["output"]["binary"]["path"] = config.output.binary.path;
    root["output"]["binary"]["quantize_embeddings"] = config.output.binary.quantize_embeddings;
    root["output"]["zmq"]["endpoint"] = config.output.zmq.endpoint;

    root["metrics"]["enabled"] = config.metrics.enabled;
    root["metrics"]["http_port"] = config.metrics.http_port;

    root["log"]["level"] = config.log.level;
    root["log"]["file"] = config.log.file;
    root["log"]["console"] = config.log.console;
    root["log"]["max_size_bytes"] = config.log.max_size_bytes;
    root["log"]["max_files"] = config.log.max_files;

    return YAML::Dump(root) + "\n";
}

} // namespace tmcmt
