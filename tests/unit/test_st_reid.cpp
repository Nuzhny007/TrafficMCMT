#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/reid/GalleryManager.h"
#include "trafficmcmt/reid/ReIdMatcher.h"
#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/PairCandidateGenerator.h"
#include "trafficmcmt/st/Polygon.h"
#include "trafficmcmt/st/SpatialGate.h"
#include "trafficmcmt/st/StReIdScorer.h"
#include "trafficmcmt/st/TrackletEndpoint.h"
#include "trafficmcmt/st/TransitionEstimator.h"

namespace {

using namespace tmcmt;
using namespace tmcmt::reid;
using namespace tmcmt::st;

Timestamp Ms(std::int64_t value) {
    return Timestamp(value);
}

WorldPoint World(double x, double y) {
    WorldPoint point;
    point.x = static_cast<float>(x);
    point.y = static_cast<float>(y);
    point.valid = true;
    return point;
}

TrackletEndpoint Endpoint(TrackletId id, const CameraId& camera, std::int64_t begin,
                          std::int64_t end, WorldPoint entry, WorldPoint exit) {
    TrackletEndpoint endpoint;
    endpoint.id = id;
    endpoint.camera = camera;
    endpoint.type = ObjectType::Car;
    endpoint.begin = Ms(begin);
    endpoint.end = Ms(end);
    endpoint.entry = entry;
    endpoint.exit = exit;
    return endpoint;
}

Tracklet MakeTracklet(TrackletId id, const CameraId& camera, std::int64_t begin,
                      std::int64_t end, std::vector<float> values, std::uint32_t modelId = 1,
                      WorldPoint entry = World(0, 0), WorldPoint exit = World(0, 0)) {
    Tracklet tracklet;
    tracklet.id = id;
    tracklet.session_id = "session";
    tracklet.camera = camera;
    tracklet.local_track_id = id;
    tracklet.sequence = 0;
    tracklet.begin = Ms(begin);
    tracklet.end = Ms(end);
    tracklet.type = ObjectType::Car;
    tracklet.quality = 1.f;
    tracklet.embedding.values = std::move(values);
    tracklet.embedding.dim = static_cast<std::uint16_t>(tracklet.embedding.values.size());
    tracklet.embedding.model_id = modelId;

    Observation first;
    first.ts = tracklet.begin;
    first.world = entry;
    tracklet.observations.push_back(first);
    if (end > begin) {
        Observation last;
        last.ts = tracklet.end;
        last.world = exit;
        tracklet.observations.push_back(last);
    }
    return tracklet;
}

std::vector<float> Normalize(std::vector<float> values) {
    double norm = 0.0;
    for (const float value : values) {
        norm += static_cast<double>(value) * value;
    }
    norm = std::sqrt(norm);
    if (norm > 0.0) {
        for (float& value : values) {
            value = static_cast<float>(value / norm);
        }
    }
    return values;
}

std::unique_ptr<GalleryManager> CreateGallery(int dim) {
    GalleryOptions options;
    options.index.dim = dim;
    auto result = GalleryManager::Create(IndexKind::Brute, options);
    if (!result) {
        return nullptr;
    }
    return std::move(result).value();
}

GalleryEntry Entry(TrackletId id, const CameraId& camera, std::vector<float> values,
                   std::int64_t begin = 0, std::int64_t end = 1000,
                   std::uint32_t modelId = 1) {
    GalleryEntry entry;
    entry.id = id;
    entry.camera = camera;
    entry.values = Normalize(std::move(values));
    entry.model_id = modelId;
    entry.type = ObjectType::Car;
    entry.begin = Ms(begin);
    entry.end = Ms(end);
    return entry;
}

class TempFile {
public:
    explicit TempFile(const std::string& name) {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("tmcmt_st_" + std::to_string(stamp) + "_" + std::to_string(counter++) + "_" + name);
        m_pathString = m_path.string();
    }

    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    [[nodiscard]] const std::string& Path() const {
        return m_pathString;
    }

private:
    std::filesystem::path m_path;
    std::string m_pathString;
};

CameraLinkModel LinkedModel() {
    CameraLinkModel model;
    model.SetDefaultLinked(false);
    CameraLink link;
    link.from = "a";
    link.to = "b";
    link.transition = TransitionStats{10000.0, 1000.0, 10};
    EXPECT_TRUE(model.SetLink(link));
    return model;
}

CameraLinkModel LinkedModelWithoutStats() {
    CameraLinkModel model;
    model.SetDefaultLinked(false);
    CameraLink link;
    link.from = "a";
    link.to = "b";
    EXPECT_TRUE(model.SetLink(link));
    return model;
}

// ---------------------------------------------------------------------------
// CameraLinkModel
// ---------------------------------------------------------------------------

TEST(CameraLinkModel, LoadsLinksZonesAndFallback) {
    const char* yaml = R"yaml(
default_linked: false
fallback:
  min_transition_ms: 100
  max_transition_ms: 50000
cameras:
  cam-1:
    exit_zone: [[0,0],[10,0],[10,10],[0,10]]
  cam-2:
    entry_zone: [[20,0],[30,0],[30,10],[20,10]]
links:
  - from: cam-1
    to: cam-2
    mean_ms: 5000
    stddev_ms: 500
    samples: 42
  - from: cam-2
    to: cam-1
    enabled: false
)yaml";

    auto result = CameraLinkModel::LoadFromString(yaml);
    ASSERT_TRUE(result) << result.status().ToString();
    const CameraLinkModel& model = result.value();

    EXPECT_FALSE(model.DefaultLinked());
    EXPECT_EQ(model.Fallback().min_ms, 100);
    EXPECT_EQ(model.Fallback().max_ms, 50000);
    EXPECT_TRUE(model.IsLinked("cam-1", "cam-2"));
    EXPECT_FALSE(model.IsLinked("cam-2", "cam-1"));
    EXPECT_FALSE(model.IsLinked("cam-1", "cam-3"));
    EXPECT_FALSE(model.IsLinked("cam-1", "cam-1"));

    const auto transition = model.Transition("cam-1", "cam-2");
    ASSERT_TRUE(transition.has_value());
    EXPECT_DOUBLE_EQ(transition->mean_ms, 5000.0);
    EXPECT_DOUBLE_EQ(transition->stddev_ms, 500.0);
    EXPECT_EQ(transition->samples, 42);
    EXPECT_FALSE(model.Transition("cam-2", "cam-1").has_value());

    const CameraZones* zones = model.Zones("cam-1");
    ASSERT_NE(zones, nullptr);
    EXPECT_TRUE(zones->exit_zone.Contains(5.0, 5.0));
    EXPECT_FALSE(zones->entry_zone.IsValid());
    EXPECT_EQ(model.Cameras().size(), 2u);
}

TEST(CameraLinkModel, DefaultLinkedAllowsUnlistedPairs) {
    const char* yaml = R"yaml(
default_linked: true
links:
  - from: a
    to: b
    enabled: false
)yaml";
    auto result = CameraLinkModel::LoadFromString(yaml);
    ASSERT_TRUE(result) << result.status().ToString();
    const CameraLinkModel& model = result.value();

    EXPECT_FALSE(model.IsLinked("a", "b"));
    EXPECT_TRUE(model.IsLinked("a", "c"));
    EXPECT_TRUE(model.IsLinked("c", "a"));
    EXPECT_FALSE(model.IsLinked("a", "a"));
}

TEST(CameraLinkModel, LinkWithoutStatisticsUsesFallback) {
    const char* yaml = R"yaml(
default_linked: false
links:
  - from: a
    to: b
)yaml";
    auto result = CameraLinkModel::LoadFromString(yaml);
    ASSERT_TRUE(result) << result.status().ToString();
    EXPECT_TRUE(result.value().IsLinked("a", "b"));
    EXPECT_FALSE(result.value().Transition("a", "b").has_value());
}

TEST(CameraLinkModel, RejectsInvalidDocuments) {
    EXPECT_FALSE(CameraLinkModel::LoadFromString("- a\n- b\n"));
    EXPECT_FALSE(CameraLinkModel::LoadFromString(R"yaml(
links:
  - from: a
    to: a
)yaml"));
    EXPECT_FALSE(CameraLinkModel::LoadFromString(R"yaml(
links:
  - from: a
    to: b
  - from: a
    to: b
)yaml"));
    EXPECT_FALSE(CameraLinkModel::LoadFromString(R"yaml(
cameras:
  a:
    entry_zone: [[0,0],[1,1]]
)yaml"));
    EXPECT_FALSE(CameraLinkModel::LoadFromString(R"yaml(
links:
  - from: a
    to: b
    mean_ms: 100
)yaml"));
    EXPECT_FALSE(CameraLinkModel::LoadFromString(R"yaml(
fallback:
  min_transition_ms: 500
  max_transition_ms: 100
)yaml"));
}

TEST(CameraLinkModel, SupportsFlatPolygonLists) {
    const char* yaml = R"yaml(
cameras:
  a:
    entry_zone: [0,0, 10,0, 10,10, 0,10]
)yaml";
    auto result = CameraLinkModel::LoadFromString(yaml);
    ASSERT_TRUE(result) << result.status().ToString();
    const CameraZones* zones = result.value().Zones("a");
    ASSERT_NE(zones, nullptr);
    ASSERT_TRUE(zones->entry_zone.IsValid());
    EXPECT_EQ(zones->entry_zone.points.size(), 4u);
    EXPECT_TRUE(zones->entry_zone.Contains(5.0, 5.0));
}

TEST(CameraLinkModel, SaveLoadRoundTrip) {
    CameraLinkModel model;
    model.SetDefaultLinked(true);
    model.SetFallback(TransitionWindow{100, 20000});

    CameraLink link;
    link.from = "a";
    link.to = "b";
    link.transition = TransitionStats{1500.0, 120.0, 7};
    ASSERT_TRUE(model.SetLink(link));

    CameraZones zones;
    zones.entry_zone.points = {EnuPoint{0, 0}, EnuPoint{10, 0}, EnuPoint{10, 10}};
    zones.exit_zone.points = {EnuPoint{20, 0}, EnuPoint{30, 0}, EnuPoint{30, 10}};
    ASSERT_TRUE(model.SetZones("a", zones));

    TempFile file("links.yaml");
    const Status saved = model.Save(file.Path());
    ASSERT_TRUE(saved) << saved.ToString();

    auto loaded = CameraLinkModel::Load(file.Path());
    ASSERT_TRUE(loaded) << loaded.status().ToString();
    EXPECT_TRUE(loaded.value().DefaultLinked());
    EXPECT_EQ(loaded.value().Fallback().min_ms, 100);
    EXPECT_EQ(loaded.value().Fallback().max_ms, 20000);
    ASSERT_TRUE(loaded.value().Transition("a", "b").has_value());
    EXPECT_DOUBLE_EQ(loaded.value().Transition("a", "b")->mean_ms, 1500.0);
    ASSERT_NE(loaded.value().Zones("a"), nullptr);
    EXPECT_TRUE(loaded.value().Zones("a")->entry_zone.Contains(5.0, 5.0));
}

// ---------------------------------------------------------------------------
// Polygon
// ---------------------------------------------------------------------------

Polygon Square() {
    Polygon polygon;
    polygon.points = {EnuPoint{0, 0}, EnuPoint{10, 0}, EnuPoint{10, 10}, EnuPoint{0, 10}};
    return polygon;
}

TEST(Polygon, InsideOutsideAndBoundary) {
    const Polygon polygon = Square();
    EXPECT_TRUE(polygon.Contains(5.0, 5.0));
    EXPECT_TRUE(polygon.Contains(0.0, 5.0));
    EXPECT_TRUE(polygon.Contains(10.0, 10.0));
    EXPECT_FALSE(polygon.Contains(15.0, 5.0));
    EXPECT_FALSE(polygon.Contains(-1.0, 5.0));
}

TEST(Polygon, ConcavePolygon) {
    Polygon polygon;
    polygon.points = {EnuPoint{0, 0},  EnuPoint{10, 0}, EnuPoint{10, 4},
                      EnuPoint{4, 4},  EnuPoint{4, 10}, EnuPoint{0, 10}};
    EXPECT_TRUE(polygon.Contains(2.0, 2.0));
    EXPECT_TRUE(polygon.Contains(8.0, 2.0));
    EXPECT_FALSE(polygon.Contains(8.0, 8.0));
}

TEST(Polygon, InvalidInput) {
    const Polygon empty;
    EXPECT_FALSE(empty.Contains(0.0, 0.0));

    Polygon line;
    line.points = {EnuPoint{0, 0}, EnuPoint{10, 10}};
    EXPECT_FALSE(line.Contains(5.0, 5.0));
    EXPECT_FALSE(Square().Contains(std::nan(""), 5.0));
}

// ---------------------------------------------------------------------------
// SpatialGate
// ---------------------------------------------------------------------------

TEST(SpatialGate, RejectsSameCamera) {
    const SpatialGate gate(nullptr, SpatialGateOptions{});
    const GateDecision decision =
        gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                   Endpoint(2, "a", 2000, 3000, World(0, 0), World(0, 0)));
    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.reason, GateReason::SameCamera);
}

TEST(SpatialGate, RejectsUnlinkedCameras) {
    const CameraLinkModel model = LinkedModel();
    SpatialGateOptions options;
    options.window = TransitionWindow{0, 120000};
    const SpatialGate gate(&model, options);
    const GateDecision decision =
        gate.Check(Endpoint(1, "b", 0, 1000, World(0, 0), World(0, 0)),
                   Endpoint(2, "a", 11000, 12000, World(0, 0), World(0, 0)));
    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.reason, GateReason::Unlinked);
}

TEST(SpatialGate, WindowAndSigmaGates) {
    const CameraLinkModel model = LinkedModel();
    SpatialGateOptions options;
    options.window = TransitionWindow{0, 120000};
    const SpatialGate gate(&model, options);

    EXPECT_TRUE(gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                           Endpoint(2, "b", 11000, 12000, World(0, 0), World(0, 0)))
                    .allowed);

    const GateDecision window =
        gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                   Endpoint(2, "b", 200000, 201000, World(0, 0), World(0, 0)));
    EXPECT_EQ(window.reason, GateReason::OutsideWindow);

    const GateDecision sigma =
        gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                   Endpoint(2, "b", 21000, 22000, World(0, 0), World(0, 0)));
    EXPECT_EQ(sigma.reason, GateReason::OutsideSigma);
}

TEST(SpatialGate, ZoneGate) {
    CameraLinkModel model = LinkedModel();
    CameraZones zones;
    zones.exit_zone.points = {EnuPoint{0, 0}, EnuPoint{10, 0}, EnuPoint{10, 10}};
    ASSERT_TRUE(model.SetZones("a", zones));
    CameraZones entryZones;
    entryZones.entry_zone.points = {EnuPoint{90, 0}, EnuPoint{110, 0}, EnuPoint{110, 10}};
    ASSERT_TRUE(model.SetZones("b", entryZones));

    SpatialGateOptions options;
    options.mode = GateMode::Zone;
    options.window = TransitionWindow{0, 120000};
    const SpatialGate gate(&model, options);

    EXPECT_TRUE(gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(5, 5)),
                           Endpoint(2, "b", 11000, 12000, World(100, 5), World(0, 0)))
                    .allowed);

    const GateDecision exit =
        gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(50, 50)),
                   Endpoint(2, "b", 11000, 12000, World(100, 5), World(0, 0)));
    EXPECT_EQ(exit.reason, GateReason::ExitZone);

    const GateDecision entry =
        gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(5, 5)),
                   Endpoint(2, "b", 11000, 12000, World(500, 5), World(0, 0)));
    EXPECT_EQ(entry.reason, GateReason::EntryZone);
}

TEST(SpatialGate, ZoneGateRequiresZonesAndWorld) {
    const CameraLinkModel model = LinkedModel();
    SpatialGateOptions options;
    options.mode = GateMode::Zone;
    options.window = TransitionWindow{0, 120000};
    const SpatialGate gate(&model, options);

    const GateDecision missingZone =
        gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(5, 5)),
                   Endpoint(2, "b", 11000, 12000, World(100, 5), World(0, 0)));
    EXPECT_EQ(missingZone.reason, GateReason::MissingZone);

    TrackletEndpoint noWorld = Endpoint(1, "a", 0, 1000, WorldPoint{}, WorldPoint{});
    const GateDecision missingWorld =
        gate.Check(noWorld, Endpoint(2, "b", 11000, 12000, World(100, 5), World(0, 0)));
    EXPECT_EQ(missingWorld.reason, GateReason::MissingWorld);
}

TEST(SpatialGate, BevGate) {
    const CameraLinkModel model = LinkedModelWithoutStats();
    SpatialGateOptions options;
    options.mode = GateMode::Bev;
    options.window = TransitionWindow{0, 120000};
    options.max_speed_mps = 30.0;
    options.max_distance_m = 200.0;
    const SpatialGate gate(&model, options);

    EXPECT_TRUE(gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                           Endpoint(2, "b", 11000, 12000, World(100, 0), World(0, 0)))
                    .allowed);

    const GateDecision speed =
        gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                   Endpoint(2, "b", 2000, 3000, World(100, 0), World(0, 0)));
    EXPECT_EQ(speed.reason, GateReason::Speed);

    const GateDecision distance =
        gate.Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                   Endpoint(2, "b", 11000, 12000, World(300, 0), World(0, 0)));
    EXPECT_EQ(distance.reason, GateReason::Distance);
}

TEST(SpatialGate, BevMissingWorldAndZeroDelta) {
    const CameraLinkModel model = LinkedModel();
    SpatialGateOptions options;
    options.mode = GateMode::Bev;
    options.window = TransitionWindow{0, 120000};
    const SpatialGate gate(&model, options);

    const TrackletEndpoint noWorld = Endpoint(1, "a", 0, 1000, WorldPoint{}, WorldPoint{});
    EXPECT_EQ(gate.Check(noWorld, Endpoint(2, "b", 11000, 12000, World(0, 0), World(0, 0)))
                  .reason,
              GateReason::MissingWorld);

    SpatialGateOptions relaxed = options;
    relaxed.allow_missing_world = true;
    const SpatialGate relaxedGate(&model, relaxed);
    EXPECT_TRUE(relaxedGate
                    .Check(noWorld, Endpoint(2, "b", 11000, 12000, World(0, 0), World(0, 0)))
                    .allowed);

    const CameraLinkModel noStats = LinkedModelWithoutStats();
    SpatialGateOptions zeroOptions;
    zeroOptions.mode = GateMode::Bev;
    zeroOptions.window = TransitionWindow{0, 120000};
    const SpatialGate zeroGate(&noStats, zeroOptions);
    EXPECT_TRUE(zeroGate
                    .Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                           Endpoint(2, "b", 1000, 1000, World(0, 0), World(0, 0)))
                    .allowed);
    EXPECT_EQ(zeroGate
                  .Check(Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                         Endpoint(2, "b", 1000, 1000, World(5, 0), World(0, 0)))
                  .reason,
              GateReason::Speed);
}

// ---------------------------------------------------------------------------
// StReIdScorer
// ---------------------------------------------------------------------------

TEST(StReIdScorer, AppearanceCostIsClampedAndSymmetric) {
    EXPECT_FLOAT_EQ(StReIdScorer::AppearanceCost(1.0f), 0.f);
    EXPECT_FLOAT_EQ(StReIdScorer::AppearanceCost(0.8f), 0.2f);
    EXPECT_FLOAT_EQ(StReIdScorer::AppearanceCost(0.0f), 1.f);
    EXPECT_FLOAT_EQ(StReIdScorer::AppearanceCost(-0.5f), 1.5f);
    EXPECT_FLOAT_EQ(StReIdScorer::AppearanceCost(-2.0f), 2.f);
}

TEST(StReIdScorer, TemporalCostIsMonotonicAndNormalised) {
    const TransitionWindow window{0, 120000};
    const TransitionStats stats{10000.0, 1000.0, 50};
    const float mean = StReIdScorer::TemporalCost(10000, window, stats);
    const float twoSigma = StReIdScorer::TemporalCost(12000, window, stats);
    const float threeSigma = StReIdScorer::TemporalCost(13000, window, stats);

    EXPECT_NEAR(mean, 0.f, 1e-6f);
    EXPECT_GT(twoSigma, mean);
    EXPECT_GT(threeSigma, twoSigma);
    EXPECT_NEAR(threeSigma, 1.f, 1e-6f);
}

TEST(StReIdScorer, TemporalCostUsesFallbackWindow) {
    const TransitionWindow window{0, 120000};
    EXPECT_NEAR(StReIdScorer::TemporalCost(60000, window, std::nullopt), 0.f, 1e-6f);
    EXPECT_NEAR(StReIdScorer::TemporalCost(0, window, std::nullopt), 1.f, 1e-6f);
    EXPECT_NEAR(StReIdScorer::TemporalCost(120000, window, std::nullopt), 1.f, 1e-6f);
}

TEST(StReIdScorer, SpatialCost) {
    EXPECT_NEAR(StReIdScorer::SpatialCost(10000, World(0, 0), World(0, 0), 30.0), 0.f, 1e-6f);
    EXPECT_NEAR(StReIdScorer::SpatialCost(1000, World(0, 0), World(30, 0), 30.0), 1.f, 1e-6f);
    EXPECT_NEAR(StReIdScorer::SpatialCost(1000, World(0, 0), World(300, 0), 30.0), 1.f, 1e-6f);
    EXPECT_NEAR(StReIdScorer::SpatialCost(0, World(0, 0), World(0, 0), 30.0), 0.f, 1e-6f);
    EXPECT_NEAR(StReIdScorer::SpatialCost(0, World(0, 0), World(1, 0), 30.0), 1.f, 1e-6f);
    EXPECT_NEAR(StReIdScorer::SpatialCost(1000, WorldPoint{}, World(0, 0), 30.0), 1.f, 1e-6f);
}

TEST(StReIdScorer, TotalIsWeightedAndNormalised) {
    ScorerOptions options;
    options.weights = StWeights{1.0, 0.0, 0.0};
    options.window = TransitionWindow{0, 120000};
    const StReIdScorer scorer(options);
    const StCosts costs =
        scorer.Score(0.8f, Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                     Endpoint(2, "b", 11000, 12000, World(10, 0), World(0, 0)));
    EXPECT_NEAR(costs.total, 0.2f, 1e-6f);

    ScorerOptions zero;
    zero.weights = StWeights{0.0, 0.0, 0.0};
    const StReIdScorer zeroScorer(zero);
    const StCosts zeroCosts =
        zeroScorer.Score(0.5f, Endpoint(1, "a", 0, 1000, World(0, 0), World(0, 0)),
                         Endpoint(2, "b", 11000, 12000, World(10, 0), World(0, 0)));
    EXPECT_NEAR(zeroCosts.total, 0.5f, 1e-6f);
}

TEST(StReIdScorer, MissingWorldSetsSpatialToOne) {
    ScorerOptions options;
    options.weights = StWeights{1.0, 0.0, 1.0};
    const StReIdScorer scorer(options);
    const StCosts costs =
        scorer.Score(1.0f, Endpoint(1, "a", 0, 1000, WorldPoint{}, WorldPoint{}),
                     Endpoint(2, "b", 11000, 12000, WorldPoint{}, WorldPoint{}));
    EXPECT_FALSE(costs.has_world);
    EXPECT_FLOAT_EQ(costs.spatial, 1.f);
    EXPECT_NEAR(costs.total, 0.5f, 1e-6f);
}

// ---------------------------------------------------------------------------
// TransitionEstimator
// ---------------------------------------------------------------------------

std::vector<TransitionSample> NormalSamples(double mean, double sigma, std::size_t count,
                                            std::uint32_t seed = 7) {
    std::mt19937 generator(seed);
    std::normal_distribution<double> distribution(mean, sigma);
    std::vector<TransitionSample> samples;
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        TransitionSample sample;
        sample.from = "a";
        sample.to = "b";
        sample.from_end = Ms(0);
        sample.to_begin = Ms(static_cast<std::int64_t>(std::llround(distribution(generator))));
        samples.push_back(sample);
    }
    return samples;
}

TEST(TransitionEstimator, RecoversKnownParameters) {
    const auto samples = NormalSamples(5000.0, 800.0, 500);
    auto result = TransitionEstimator::Estimate(samples);
    ASSERT_TRUE(result) << result.status().ToString();
    ASSERT_EQ(result.value().size(), 1u);
    const TransitionStats& stats = *result.value()[0].transition;
    EXPECT_NEAR(stats.mean_ms, 5000.0, 0.15 * 5000.0);
    EXPECT_NEAR(stats.stddev_ms, 800.0, 0.15 * 800.0);
    EXPECT_GE(stats.samples, 490);
}

TEST(TransitionEstimator, TrimsOutliers) {
    std::vector<TransitionSample> samples;
    for (const std::int64_t delta : {100, 110, 120, 130, 140, 10000}) {
        TransitionSample sample;
        sample.from = "a";
        sample.to = "b";
        sample.from_end = Ms(0);
        sample.to_begin = Ms(delta);
        samples.push_back(sample);
    }
    EstimatorOptions options;
    options.min_samples = 3;
    auto result = TransitionEstimator::Estimate(samples, options);
    ASSERT_TRUE(result) << result.status().ToString();
    ASSERT_EQ(result.value().size(), 1u);
    const TransitionStats& stats = *result.value()[0].transition;
    EXPECT_NEAR(stats.mean_ms, 120.0, 1e-6);
    EXPECT_EQ(stats.samples, 5);
    EXPECT_LT(stats.mean_ms, 1000.0);
}

TEST(TransitionEstimator, EvenSampleMedian) {
    std::vector<TransitionSample> samples;
    for (const std::int64_t delta : {100, 200, 300, 400}) {
        TransitionSample sample;
        sample.from = "a";
        sample.to = "b";
        sample.from_end = Ms(0);
        sample.to_begin = Ms(delta);
        samples.push_back(sample);
    }
    EstimatorOptions options;
    options.outlier_sigma = 0.0;
    auto result = TransitionEstimator::Estimate(samples, options);
    ASSERT_TRUE(result) << result.status().ToString();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_NEAR(result.value()[0].transition->mean_ms, 250.0, 1e-6);
}

TEST(TransitionEstimator, FiltersInvalidSamples) {
    std::vector<TransitionSample> samples;
    for (const std::int64_t delta : {-100, 100, 110, 120}) {
        TransitionSample sample;
        sample.from = "a";
        sample.to = "b";
        sample.from_end = Ms(0);
        sample.to_begin = Ms(delta);
        samples.push_back(sample);
    }
    EstimatorOptions options;
    options.min_samples = 4;
    auto result = TransitionEstimator::Estimate(samples, options);
    ASSERT_TRUE(result) << result.status().ToString();
    EXPECT_TRUE(result.value().empty());
}

TEST(TransitionEstimator, EmptyInputAndOptionValidation) {
    auto empty = TransitionEstimator::Estimate({});
    ASSERT_TRUE(empty) << empty.status().ToString();
    EXPECT_TRUE(empty.value().empty());

    EstimatorOptions options;
    options.min_samples = 0;
    EXPECT_FALSE(TransitionEstimator::Estimate({}, options));

    options.min_samples = 3;
    options.mad_scale = 0.0;
    EXPECT_FALSE(TransitionEstimator::Estimate({}, options));

    options.mad_scale = 1.4826;
    options.min_stddev_ms = 0.0;
    EXPECT_FALSE(TransitionEstimator::Estimate({}, options));

    options.min_stddev_ms = 1.0;
    options.outlier_sigma = -1.0;
    EXPECT_FALSE(TransitionEstimator::Estimate({}, options));
}

TEST(TransitionEstimator, GroupsByDirectedCameraPair) {
    std::vector<TransitionSample> samples;
    for (int i = 0; i < 5; ++i) {
        TransitionSample forward;
        forward.from = "a";
        forward.to = "b";
        forward.from_end = Ms(0);
        forward.to_begin = Ms(1000 + i);
        samples.push_back(forward);

        TransitionSample backward;
        backward.from = "b";
        backward.to = "a";
        backward.from_end = Ms(0);
        backward.to_begin = Ms(2000 + i);
        samples.push_back(backward);
    }
    auto result = TransitionEstimator::Estimate(samples);
    ASSERT_TRUE(result) << result.status().ToString();
    ASSERT_EQ(result.value().size(), 2u);
    EXPECT_EQ(result.value()[0].from, "a");
    EXPECT_EQ(result.value()[1].from, "b");
}

// ---------------------------------------------------------------------------
// TrackletEndpoint and gallery metadata
// ---------------------------------------------------------------------------

TEST(TrackletEndpoint, UsesFirstAndLastValidWorldPoint) {
    Tracklet tracklet = MakeTracklet(1, "a", 0, 1000, {1.f});
    tracklet.observations.clear();
    Observation invalid;
    invalid.ts = Ms(0);
    tracklet.observations.push_back(invalid);
    Observation first;
    first.ts = Ms(100);
    first.world = World(5, 6);
    tracklet.observations.push_back(first);
    Observation middleInvalid;
    middleInvalid.ts = Ms(200);
    tracklet.observations.push_back(middleInvalid);
    Observation last;
    last.ts = Ms(300);
    last.world = World(7, 8);
    tracklet.observations.push_back(last);

    const TrackletEndpoint endpoint = MakeEndpoint(tracklet);
    ASSERT_TRUE(endpoint.HasWorld());
    EXPECT_FLOAT_EQ(endpoint.entry.x, 5.f);
    EXPECT_FLOAT_EQ(endpoint.entry.y, 6.f);
    EXPECT_FLOAT_EQ(endpoint.exit.x, 7.f);
    EXPECT_FLOAT_EQ(endpoint.exit.y, 8.f);
}

TEST(TrackletEndpoint, NoWorldCoordinates) {
    Tracklet tracklet = MakeTracklet(1, "a", 0, 1000, {1.f});
    tracklet.observations.clear();
    Observation observation;
    observation.ts = Ms(0);
    tracklet.observations.push_back(observation);
    EXPECT_FALSE(MakeEndpoint(tracklet).HasWorld());
}

TEST(GalleryManager, PropagatesWorldEndpoints) {
    auto gallery = CreateGallery(1);
    ASSERT_NE(gallery, nullptr);

    Tracklet tracklet = MakeTracklet(11, "a", 0, 1000, {1.f}, 1, World(1, 2), World(3, 4));
    ASSERT_TRUE(gallery->Add(tracklet));

    const GalleryEntry* entry = gallery->Get(11);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->entry_world.IsFinite());
    ASSERT_TRUE(entry->exit_world.IsFinite());
    EXPECT_FLOAT_EQ(entry->entry_world.x, 1.f);
    EXPECT_FLOAT_EQ(entry->entry_world.y, 2.f);
    EXPECT_FLOAT_EQ(entry->exit_world.x, 3.f);
    EXPECT_FLOAT_EQ(entry->exit_world.y, 4.f);

    const TrackletEndpoint endpoint = MakeEndpoint(*entry);
    EXPECT_TRUE(endpoint.HasWorld());
    EXPECT_FLOAT_EQ(endpoint.entry.x, 1.f);
    EXPECT_FLOAT_EQ(endpoint.exit.x, 3.f);
}

// ---------------------------------------------------------------------------
// PairCandidateGenerator
// ---------------------------------------------------------------------------

TEST(PairCandidateGenerator, SortsByTotalCostAndTruncates) {
    auto gallery = CreateGallery(2);
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(10, "b", {1.f, 0.f}, 1000, 2000)));
    ASSERT_TRUE(gallery->Add(Entry(11, "b", {0.9f, 0.1f}, 5000, 6000)));

    const CameraLinkModel model = LinkedModelWithoutStats();
    PairGenerationOptions options;
    options.top_k = 1;
    options.oversample = 4;
    options.window = TransitionWindow{0, 10000};
    options.gate_mode = GateMode::Off;
    options.max_total_cost = 1.0;
    PairCandidateGenerator generator(*gallery, &model, options);

    const Tracklet query = MakeTracklet(1, "a", 0, 0, {1.f, 0.f});
    auto result = generator.Generate(query);
    ASSERT_TRUE(result) << result.status().ToString();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0].candidate_id, 11u);
    EXPECT_EQ(generator.Stats().accepted, 1u);
    EXPECT_EQ(generator.Stats().truncated, 1u);
}

TEST(PairCandidateGenerator, AppliesMinSimilarity) {
    auto gallery = CreateGallery(2);
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(10, "b", {1.f, 0.f}, 1000, 2000)));
    ASSERT_TRUE(gallery->Add(Entry(11, "b", {0.9f, 0.1f}, 5000, 6000)));

    const CameraLinkModel model = LinkedModelWithoutStats();
    PairGenerationOptions options;
    options.top_k = 5;
    options.min_similarity = 0.995;
    options.window = model.Fallback();
    PairCandidateGenerator generator(*gallery, &model, options);

    auto result = generator.Generate(MakeTracklet(1, "a", 0, 0, {1.f, 0.f}));
    ASSERT_TRUE(result) << result.status().ToString();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0].candidate_id, 10u);
}

TEST(PairCandidateGenerator, AppliesMaxTotalCost) {
    auto gallery = CreateGallery(2);
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(10, "b", {0.f, 1.f}, 1000, 2000)));

    const CameraLinkModel model = LinkedModelWithoutStats();
    PairGenerationOptions options;
    options.top_k = 5;
    options.window = model.Fallback();
    options.max_total_cost = 0.01;
    PairCandidateGenerator generator(*gallery, &model, options);

    auto result = generator.Generate(MakeTracklet(1, "a", 0, 0, {1.f, 0.f}));
    ASSERT_TRUE(result) << result.status().ToString();
    EXPECT_TRUE(result.value().empty());
    EXPECT_GT(generator.Stats().rejected_cost, 0u);
}

TEST(PairCandidateGenerator, ExcludesSameCamera) {
    auto gallery = CreateGallery(2);
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(10, "a", {1.f, 0.f}, 1000, 2000)));
    ASSERT_TRUE(gallery->Add(Entry(11, "b", {1.f, 0.f}, 1000, 2000)));

    CameraLinkModel model;
    model.SetDefaultLinked(true);
    PairGenerationOptions options;
    options.top_k = 5;
    options.window = model.Fallback();
    PairCandidateGenerator generator(*gallery, &model, options);

    auto result = generator.Generate(MakeTracklet(1, "a", 0, 0, {1.f, 0.f}));
    ASSERT_TRUE(result) << result.status().ToString();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(result.value()[0].candidate_id, 11u);
}

TEST(PairCandidateGenerator, ReportsUnlinkedAndMissingWorld) {
    auto gallery = CreateGallery(2);
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(10, "b", {1.f, 0.f}, 1000, 2000)));

    CameraLinkModel model;
    model.SetDefaultLinked(false);
    PairGenerationOptions options;
    options.top_k = 5;
    options.window = model.Fallback();
    PairCandidateGenerator generator(*gallery, &model, options);

    auto result = generator.Generate(MakeTracklet(1, "a", 0, 0, {1.f, 0.f}));
    ASSERT_TRUE(result) << result.status().ToString();
    EXPECT_TRUE(result.value().empty());
    EXPECT_GT(generator.Stats().rejected_unlinked, 0u);

    model.SetDefaultLinked(true);
    options.gate_mode = GateMode::Bev;
    PairCandidateGenerator bevGenerator(*gallery, &model, options);
    auto bevResult = bevGenerator.Generate(MakeTracklet(1, "a", 0, 0, {1.f, 0.f}));
    ASSERT_TRUE(bevResult) << bevResult.status().ToString();
    EXPECT_TRUE(bevResult.value().empty());
    EXPECT_GT(bevGenerator.Stats().rejected_missing_world, 0u);
}

TEST(PairCandidateGenerator, GeneratesForMultipleQueries) {
    auto gallery = CreateGallery(2);
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(10, "b", {1.f, 0.f}, 1000, 2000)));
    ASSERT_TRUE(gallery->Add(Entry(11, "b", {0.f, 1.f}, 3000, 4000)));

    const CameraLinkModel model = LinkedModelWithoutStats();
    PairGenerationOptions options;
    options.top_k = 1;
    options.window = model.Fallback();
    PairCandidateGenerator generator(*gallery, &model, options);

    const std::vector<Tracklet> queries = {MakeTracklet(1, "a", 0, 0, {1.f, 0.f}),
                                           MakeTracklet(2, "a", 0, 0, {0.f, 1.f})};
    auto result = generator.Generate(queries);
    ASSERT_TRUE(result) << result.status().ToString();
    ASSERT_EQ(result.value().size(), 2u);
    EXPECT_EQ(result.value()[0].candidate_id, 10u);
    EXPECT_EQ(result.value()[1].candidate_id, 11u);
    EXPECT_EQ(generator.Stats().accepted, 2u);
}

// ---------------------------------------------------------------------------
// DoD synthetic check: 5 cameras, 50 objects, labelled transitions.
// ---------------------------------------------------------------------------

TEST(StReIdDoD, SyntheticPrecisionRecallAndFalsePositiveReduction) {
    constexpr int kObjects = 50;
    constexpr int kDim = 8;
    constexpr int kCameras = 5;

    auto gallery = CreateGallery(kDim);
    ASSERT_NE(gallery, nullptr);

    CameraLinkModel model;
    model.SetDefaultLinked(false);
    model.SetFallback(TransitionWindow{0, 120000});
    for (int camera = 0; camera < kCameras; ++camera) {
        CameraLink link;
        link.from = "cam-" + std::to_string(camera);
        link.to = "cam-" + std::to_string((camera + 1) % kCameras);
        ASSERT_TRUE(model.SetLink(link));
    }

    std::mt19937 generator(12345);
    std::normal_distribution<float> noise(0.f, 1.f);

    std::vector<Tracklet> queries;
    std::vector<TrackletId> groundTruth;
    queries.reserve(kObjects);
    groundTruth.reserve(kObjects);

    for (int object = 0; object < kObjects; ++object) {
        std::vector<float> base(static_cast<std::size_t>(kDim));
        for (float& value : base) {
            value = noise(generator);
        }
        base = Normalize(std::move(base));

        std::vector<float> queryValues = base;
        for (float& value : queryValues) {
            value += 0.05f * noise(generator);
        }
        queryValues = Normalize(std::move(queryValues));

        const CameraId queryCamera = "cam-" + std::to_string(object % kCameras);
        const CameraId targetCamera = "cam-" + std::to_string((object + 1) % kCameras);
        const std::int64_t queryBegin = 1000LL * object;
        const std::int64_t queryEnd = queryBegin + 1000;

        Tracklet query = MakeTracklet(static_cast<TrackletId>(object + 1), queryCamera,
                                      queryBegin, queryEnd, queryValues, 1, World(0, 0),
                                      World(0, 0));
        queries.push_back(query);

        const TrackletId trueId = static_cast<TrackletId>(10000 + object);
        std::vector<float> trueValues = base;
        for (float& value : trueValues) {
            value += 0.05f * noise(generator);
        }
        Tracklet trueTracklet =
            MakeTracklet(trueId, targetCamera, queryEnd + 10000, queryEnd + 11000,
                         Normalize(std::move(trueValues)), 1, World(10, 0), World(10, 0));
        ASSERT_TRUE(gallery->Add(trueTracklet));
        groundTruth.push_back(trueId);

        if (object % 2 == 0) {
            // Near-duplicate on an unlinked camera: appearance-only top-1.
            std::vector<float> confuserValues = queryValues;
            for (float& value : confuserValues) {
                value += 0.01f * noise(generator);
            }
            ASSERT_TRUE(gallery->Add(Entry(static_cast<TrackletId>(20000 + object), "cam-9",
                                           Normalize(std::move(confuserValues)),
                                           queryEnd + 10000, queryEnd + 11000)));

            // Linked camera but physically impossible: 1000 m in 1 s.
            std::vector<float> fastValues = queryValues;
            for (float& value : fastValues) {
                value += 0.02f * noise(generator);
            }
            GalleryEntry fast = Entry(static_cast<TrackletId>(30000 + object), targetCamera,
                                      Normalize(std::move(fastValues)), queryEnd + 1000,
                                      queryEnd + 2000);
            fast.entry_world = World(1000, 0);
            fast.exit_world = World(1000, 0);
            ASSERT_TRUE(gallery->Add(fast));
        }
    }

    // Appearance-only baseline: top-1 cross-camera match.
    std::size_t appearanceTruePositives = 0;
    std::size_t appearanceFalsePositives = 0;
    {
        MatcherOptions matcherOptions;
        matcherOptions.top_k = 1;
        matcherOptions.min_similarity = 0.0;
        matcherOptions.exclude_same_camera = true;
        const ReIdMatcher matcher(*gallery, matcherOptions);
        for (std::size_t i = 0; i < queries.size(); ++i) {
            auto matches = matcher.Match(queries[i]);
            ASSERT_TRUE(matches) << matches.status().ToString();
            if (matches.value().empty()) {
                continue;
            }
            if (matches.value()[0].candidate_id == groundTruth[i]) {
                ++appearanceTruePositives;
            } else {
                ++appearanceFalsePositives;
            }
        }
    }

    PairGenerationOptions options;
    options.top_k = 1;
    options.oversample = 4;
    options.min_similarity = 0.0;
    options.exclude_same_camera = true;
    options.max_total_cost = 1.0;
    options.weights = StWeights{1.0, 0.5, 0.3};
    options.window = model.Fallback();
    options.gate_mode = GateMode::Bev;
    options.max_speed_mps = 30.0;
    options.max_distance_m = 200.0;
    PairCandidateGenerator pairGenerator(*gallery, &model, options);

    auto candidates = pairGenerator.Generate(queries);
    ASSERT_TRUE(candidates) << candidates.status().ToString();

    std::size_t stTruePositives = 0;
    std::size_t stFalsePositives = 0;
    for (const PairCandidate& candidate : candidates.value()) {
        const auto index = static_cast<std::size_t>(candidate.query_id - 1);
        ASSERT_LT(index, groundTruth.size());
        if (candidate.candidate_id == groundTruth[index]) {
            ++stTruePositives;
        } else {
            ++stFalsePositives;
        }
    }

    const double appearancePrecision =
        appearanceTruePositives + appearanceFalsePositives == 0
            ? 0.0
            : static_cast<double>(appearanceTruePositives) /
                  static_cast<double>(appearanceTruePositives + appearanceFalsePositives);
    const double stPrecision =
        stTruePositives + stFalsePositives == 0
            ? 0.0
            : static_cast<double>(stTruePositives) /
                  static_cast<double>(stTruePositives + stFalsePositives);
    const double stRecall =
        static_cast<double>(stTruePositives) / static_cast<double>(queries.size());

    EXPECT_LT(appearancePrecision, stPrecision);
    EXPECT_GE(stPrecision, 0.95);
    EXPECT_GE(stRecall, 0.80);
    EXPECT_GE(appearanceFalsePositives, 3 * stFalsePositives);

    EXPECT_GT(pairGenerator.Stats().rejected_unlinked, 0u);
    EXPECT_GT(pairGenerator.Stats().rejected_distance, 0u);
    EXPECT_GT(pairGenerator.Stats().appearance_candidates, queries.size());
}

} // namespace
