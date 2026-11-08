#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "trafficmcmt/core/Config.h"

namespace {

using tmcmt::GateMode;
using tmcmt::IndexKind;
using tmcmt::MasterConfig;
using tmcmt::PipelineMode;
using tmcmt::SolverKind;
using tmcmt::SourceKind;

class TempFile {
public:
    explicit TempFile(const std::string& content, const std::string& extension = ".yaml") {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("tmcmt_test_" + std::to_string(stamp) + "_" + std::to_string(counter++) + extension);
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] std::string Path() const {
        return m_path.string();
    }

private:
    std::filesystem::path m_path;
};

const char* kFullConfig = R"yaml(
master:
  mode: online
  window_ms: 20000
  window_overlap_ms: 5000
  commit_lag_ms: 9000

ingest:
  source: zmq
  max_lateness_ms: 1500
  file:
    path: data/replay.jsonl
    speed: 2.0
  zmq:
    endpoint: tcp://0.0.0.0:6000
    subscribe_prefix: tmcmt.v1.
    rcv_hwm: 100
    reconnect_ivl_ms: 250

reid:
  embedding_dim: 256
  index: faiss_ivf_pq
  nlist: 128
  nprobe: 8
  top_k: 7
  min_similarity: 0.35
  exclude_same_camera: false
  whitening: data/whitening.bin

st:
  min_transition_ms: 100
  max_transition_ms: 60000
  weights:
    appearance: 2.0
    temporal: 0.25
    spatial: 0.75
  gate:
    mode: bev
    max_speed_mps: 12.5
    max_distance_m: 80.0

flow:
  solver: mussp
  max_candidates_per_node: 9
  max_component_nodes: 500

storage:
  backend: sqlite
  path: data/test.db
  retention_days: 3

output:
  sinks: [jsonl, zmq]
  jsonl:
    path: out/tracks.jsonl
  zmq:
    endpoint: tcp://0.0.0.0:7000

metrics:
  enabled: true
  http_port: 9200

log:
  level: debug
  file: logs/test.log
  console: false
  max_size_bytes: 1024
  max_files: 2
)yaml";

TEST(Config, LoadsFullConfiguration) {
    const TempFile file(kFullConfig);
    auto result = MasterConfig::Load(file.Path());
    ASSERT_TRUE(result) << result.status().ToString();

    const MasterConfig& config = result.value();
    EXPECT_EQ(config.mode, PipelineMode::Online);
    EXPECT_EQ(config.window_ms, 20000);
    EXPECT_EQ(config.window_overlap_ms, 5000);
    EXPECT_EQ(config.commit_lag_ms, 9000);

    EXPECT_EQ(config.ingest.source, SourceKind::Zmq);
    EXPECT_EQ(config.ingest.max_lateness_ms, 1500);
    EXPECT_EQ(config.ingest.file.path, "data/replay.jsonl");
    EXPECT_DOUBLE_EQ(config.ingest.file.speed, 2.0);
    EXPECT_EQ(config.ingest.zmq.endpoint, "tcp://0.0.0.0:6000");
    EXPECT_EQ(config.ingest.zmq.rcv_hwm, 100);
    EXPECT_EQ(config.ingest.zmq.reconnect_ivl_ms, 250);

    EXPECT_EQ(config.reid.embedding_dim, 256);
    EXPECT_EQ(config.reid.index, IndexKind::FaissIvfPq);
    EXPECT_EQ(config.reid.nlist, 128);
    EXPECT_EQ(config.reid.nprobe, 8);
    EXPECT_EQ(config.reid.top_k, 7);
    EXPECT_DOUBLE_EQ(config.reid.min_similarity, 0.35);
    EXPECT_FALSE(config.reid.exclude_same_camera);
    EXPECT_EQ(config.reid.whitening, "data/whitening.bin");

    EXPECT_EQ(config.st.min_transition_ms, 100);
    EXPECT_EQ(config.st.max_transition_ms, 60000);
    EXPECT_DOUBLE_EQ(config.st.weights.appearance, 2.0);
    EXPECT_DOUBLE_EQ(config.st.weights.temporal, 0.25);
    EXPECT_DOUBLE_EQ(config.st.weights.spatial, 0.75);
    EXPECT_EQ(config.st.gate.mode, GateMode::Bev);
    EXPECT_DOUBLE_EQ(config.st.gate.max_speed_mps, 12.5);
    EXPECT_DOUBLE_EQ(config.st.gate.max_distance_m, 80.0);

    EXPECT_EQ(config.flow.solver, SolverKind::MuSsp);
    EXPECT_EQ(config.flow.max_candidates_per_node, 9);
    EXPECT_EQ(config.flow.max_component_nodes, 500);

    EXPECT_EQ(config.storage.backend, "sqlite");
    EXPECT_EQ(config.storage.path, "data/test.db");
    EXPECT_EQ(config.storage.retention_days, 3);

    ASSERT_EQ(config.output.sinks.size(), 2u);
    EXPECT_EQ(config.output.sinks[0], "jsonl");
    EXPECT_EQ(config.output.sinks[1], "zmq");
    EXPECT_EQ(config.output.jsonl.path, "out/tracks.jsonl");
    EXPECT_EQ(config.output.zmq.endpoint, "tcp://0.0.0.0:7000");

    EXPECT_TRUE(config.metrics.enabled);
    EXPECT_EQ(config.metrics.http_port, 9200);

    EXPECT_EQ(config.log.level, "debug");
    EXPECT_EQ(config.log.file, "logs/test.log");
    EXPECT_FALSE(config.log.console);
    EXPECT_EQ(config.log.max_size_bytes, 1024u);
    EXPECT_EQ(config.log.max_files, 2u);
}

TEST(Config, AppliesDefaultsWhenSectionsAreMissing) {
    const TempFile file("master: {}\n");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_TRUE(result) << result.status().ToString();

    const MasterConfig& config = result.value();
    EXPECT_EQ(config.mode, PipelineMode::Offline);
    EXPECT_EQ(config.window_ms, 30000);
    EXPECT_EQ(config.window_overlap_ms, 10000);
    EXPECT_EQ(config.commit_lag_ms, 15000);
    EXPECT_EQ(config.ingest.source, SourceKind::File);
    EXPECT_EQ(config.ingest.file.path, "data/tracklets.jsonl");
    EXPECT_EQ(config.reid.index, IndexKind::Brute);
    EXPECT_EQ(config.reid.embedding_dim, 512);
    EXPECT_DOUBLE_EQ(config.reid.min_similarity, 0.0);
    EXPECT_TRUE(config.reid.exclude_same_camera);
    EXPECT_EQ(config.st.gate.mode, GateMode::Off);
    EXPECT_EQ(config.flow.solver, SolverKind::Ssp);
    EXPECT_EQ(config.storage.backend, "memory");
    ASSERT_EQ(config.output.sinks.size(), 1u);
    EXPECT_EQ(config.output.sinks[0], "jsonl");
    EXPECT_EQ(config.log.level, "info");
}

TEST(Config, IgnoresUnknownKeys) {
    const TempFile file("master:\n  mode: offline\n  future_option: 1\nunknown_section:\n  a: b\n");
    auto result = MasterConfig::Load(file.Path());
    EXPECT_TRUE(result) << result.status().ToString();
}

TEST(Config, RejectsZeroWindow) {
    const TempFile file("master:\n  window_ms: 0\n");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::InvalidArgument);
    EXPECT_NE(result.status().message().find("master.window_ms"), std::string::npos);
}

TEST(Config, RejectsOverlapNotSmallerThanWindow) {
    const TempFile file("master:\n  window_ms: 10000\n  window_overlap_ms: 10000\n");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_FALSE(result);
    EXPECT_NE(result.status().message().find("master.window_overlap_ms"), std::string::npos);
}

TEST(Config, RejectsUnknownSource) {
    const TempFile file("ingest:\n  source: kafka\n");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::ParseError);
    EXPECT_NE(result.status().message().find("ingest.source"), std::string::npos);
    EXPECT_NE(result.status().message().find("kafka"), std::string::npos);
}

TEST(Config, RejectsUnknownSolver) {
    const TempFile file("flow:\n  solver: magic\n");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_FALSE(result);
    EXPECT_NE(result.status().message().find("flow.solver"), std::string::npos);
}

TEST(Config, RejectsSimilarityOutsideCosineRange) {
    const TempFile file("reid:\n  min_similarity: 1.5\n");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::InvalidArgument);
    EXPECT_NE(result.status().message().find("reid.min_similarity"), std::string::npos);
}

TEST(Config, RejectsEmptyFileList) {
    const TempFile file("output:\n  sinks: []\n");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_FALSE(result);
    EXPECT_NE(result.status().message().find("output.sinks"), std::string::npos);
}

TEST(Config, ReportsMissingFile) {
    auto result = MasterConfig::Load("definitely_missing_trafficmcmt_config.yaml");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::IoError);
}

TEST(Config, ReportsBrokenYaml) {
    const TempFile file("master: [\n  mode: offline\n");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::ParseError);
}

TEST(Config, RejectsEmptyConfiguration) {
    const TempFile file("");
    auto result = MasterConfig::Load(file.Path());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::ParseError);
}

TEST(Config, DumpRoundTrips) {
    const TempFile original(kFullConfig);
    auto first = MasterConfig::Load(original.Path());
    ASSERT_TRUE(first) << first.status().ToString();

    const std::string dumped = tmcmt::Dump(first.value());
    const TempFile second(dumped);
    auto reloaded = MasterConfig::Load(second.Path());
    ASSERT_TRUE(reloaded) << reloaded.status().ToString();

    const MasterConfig& a = first.value();
    const MasterConfig& b = reloaded.value();
    EXPECT_EQ(a.mode, b.mode);
    EXPECT_EQ(a.window_ms, b.window_ms);
    EXPECT_EQ(a.window_overlap_ms, b.window_overlap_ms);
    EXPECT_EQ(a.commit_lag_ms, b.commit_lag_ms);
    EXPECT_EQ(a.ingest.source, b.ingest.source);
    EXPECT_EQ(a.ingest.zmq.endpoint, b.ingest.zmq.endpoint);
    EXPECT_EQ(a.reid.index, b.reid.index);
    EXPECT_EQ(a.reid.embedding_dim, b.reid.embedding_dim);
    EXPECT_DOUBLE_EQ(a.reid.min_similarity, b.reid.min_similarity);
    EXPECT_EQ(a.reid.exclude_same_camera, b.reid.exclude_same_camera);
    EXPECT_EQ(a.st.gate.mode, b.st.gate.mode);
    EXPECT_DOUBLE_EQ(a.st.weights.appearance, b.st.weights.appearance);
    EXPECT_EQ(a.flow.solver, b.flow.solver);
    EXPECT_EQ(a.storage.backend, b.storage.backend);
    EXPECT_EQ(a.output.sinks, b.output.sinks);
    EXPECT_EQ(a.metrics.http_port, b.metrics.http_port);
    EXPECT_EQ(a.log.level, b.log.level);
}

TEST(Config, LoadsStandaloneLogConfig) {
    const TempFile file("level: warn\nfile: logs/standalone.log\nconsole: true\nmax_size_bytes: 2048\nmax_files: 3\n");
    auto result = tmcmt::LoadLogConfig(file.Path());
    ASSERT_TRUE(result) << result.status().ToString();
    EXPECT_EQ(result.value().level, "warn");
    EXPECT_EQ(result.value().file, "logs/standalone.log");
    EXPECT_EQ(result.value().max_size_bytes, 2048u);
    EXPECT_EQ(result.value().max_files, 3u);
}

TEST(Config, RejectsUnknownLogLevel) {
    const TempFile file("level: verbose\n");
    auto result = tmcmt::LoadLogConfig(file.Path());
    ASSERT_FALSE(result);
    EXPECT_NE(result.status().message().find("log.level"), std::string::npos);
}

} // namespace
