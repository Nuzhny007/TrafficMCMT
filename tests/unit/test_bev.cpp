#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "trafficmcmt/bev/CameraCalibrationStore.h"
#include "trafficmcmt/bev/HomographyProjector.h"
#include "trafficmcmt/bev/HomographySolver.h"
#include "trafficmcmt/bev/WorldTrackSink.h"
#include "trafficmcmt/bev/WorldTrajectoryFusion.h"
#include "trafficmcmt/core/Camera.h"
#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/TrackletId.h"
#include "trafficmcmt/st/BeVSpatialGate.h"
#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/TrackletEndpoint.h"

namespace {

using tmcmt::BBox;
using tmcmt::CameraCalibration;
using tmcmt::CameraId;
using tmcmt::EnuPoint;
using tmcmt::GateMode;
using tmcmt::GlobalTrackUpdate;
using tmcmt::Homography;
using tmcmt::Observation;
using tmcmt::ObjectType;
using tmcmt::Timestamp;
using tmcmt::Tracklet;
using tmcmt::WorldPoint;

using tmcmt::bev::CalibrationStoreOptions;
using tmcmt::bev::CameraCalibrationStore;
using tmcmt::bev::Correspondence;
using tmcmt::bev::HomographyProjector;
using tmcmt::bev::HomographySolver;
using tmcmt::bev::ImageSize;
using tmcmt::bev::WorldFusionOptions;
using tmcmt::bev::WorldOutputFormat;
using tmcmt::bev::WorldTrackSink;
using tmcmt::bev::WorldTrajectory;
using tmcmt::bev::WorldTrajectoryFusion;
using tmcmt::st::BeVSpatialGate;
using tmcmt::st::CameraLinkModel;
using tmcmt::st::GateReason;
using tmcmt::st::SpatialGateOptions;
using tmcmt::st::TrackletEndpoint;
using tmcmt::st::TransitionWindow;

/// Temporary file with an explicit, monotonically increasing modification
/// time so hot-reload tests do not depend on filesystem timestamp resolution.
class TempFile {
public:
    explicit TempFile(std::string content, std::string extension = ".yaml") {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("tmcmt_bev_" + std::to_string(stamp) + "_" + std::to_string(counter++) + extension);
        Write(content);
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    void Write(const std::string& content) {
        {
            std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
            out << content;
        }
        std::error_code ec;
        const auto now = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(m_path, now + std::chrono::seconds(++m_bump), ec);
    }

    [[nodiscard]] std::string Path() const {
        return m_path.string();
    }

private:
    std::filesystem::path m_path;
    int m_bump = 0;
};

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Normalised homography: `(u, v)` in [0, 1] -> metres. The bottom of the image
/// maps to `north = 0`, the left edge to `east = 0`.
CameraCalibration MakeNormalizedCalibration(CameraId camera = "cam-01") {
    CameraCalibration calibration;
    calibration.camera_id = std::move(camera);
    calibration.has_homography = true;
    calibration.homography = Homography{100.0, 0.0, 0.0, 0.0, -100.0, 100.0, 0.0, 0.0, 1.0};
    calibration.image_width = 1920;
    calibration.image_height = 1080;
    calibration.homography_normalized = true;
    return calibration;
}

Tracklet MakeWorldSegment(const CameraId& camera, std::uint64_t local_id, ObjectType type,
                          const std::vector<std::pair<std::int64_t, EnuPoint>>& samples,
                          float quality = 0.9f) {
    Tracklet tracklet;
    tracklet.session_id = "bev-session";
    tracklet.camera = camera;
    tracklet.local_track_id = local_id;
    tracklet.type = type;
    tracklet.quality = quality;
    tracklet.begin = Timestamp(samples.front().first);
    tracklet.end = Timestamp(samples.back().first);
    tracklet.observations.reserve(samples.size());
    for (const auto& sample : samples) {
        Observation observation;
        observation.ts = Timestamp(sample.first);
        observation.bbox = BBox{0.f, 0.f, 10.f, 10.f, 0.9f};
        observation.world = WorldPoint{static_cast<float>(sample.second.east),
                                       static_cast<float>(sample.second.north), true};
        tracklet.observations.push_back(observation);
    }
    tmcmt::AssignTrackletId(tracklet);
    return tracklet;
}

TrackletEndpoint MakeEndpoint(const CameraId& camera, std::int64_t begin, std::int64_t end,
                              std::optional<EnuPoint> entry, std::optional<EnuPoint> exit) {
    TrackletEndpoint endpoint;
    endpoint.camera = camera;
    endpoint.begin = Timestamp(begin);
    endpoint.end = Timestamp(end);
    if (entry) {
        endpoint.entry = WorldPoint{static_cast<float>(entry->east),
                                    static_cast<float>(entry->north), true};
    }
    if (exit) {
        endpoint.exit = WorldPoint{static_cast<float>(exit->east),
                                   static_cast<float>(exit->north), true};
    }
    return endpoint;
}

const char* kCalibrationYaml = R"yaml(
calibrations:
  - camera_id: cam-01
    homography: [100, 0, 0, 0, -100, 100, 0, 0, 1]
    origin_lat: 32.606456
    origin_lon: -85.481891
    image_width: 1920
    image_height: 1080
    reprojection_error_m: 0.2
    source: manual
    version: 1
)yaml";

const char* kCalibrationYamlV2 = R"yaml(
calibrations:
  - camera_id: cam-01
    homography: [120, 0, 0, 0, -120, 120, 0, 0, 1]
    origin_lat: 32.606456
    origin_lon: -85.481891
    image_width: 1920
    image_height: 1080
    reprojection_error_m: 0.3
    source: estimated
    version: 2
)yaml";

// ---------------------------------------------------------------------------
// Homography projector
// ---------------------------------------------------------------------------

TEST(BeVProjector, NormalizedRoundTrip) {
    auto created = HomographyProjector::Create(MakeNormalizedCalibration());
    ASSERT_TRUE(created) << created.status().ToString();
    const HomographyProjector& projector = created.value();
    ASSERT_TRUE(projector.IsValid());
    EXPECT_TRUE(projector.IsNormalized());

    const std::array<std::array<double, 2>, 3> points{{{0.25, 0.25}, {0.5, 0.5}, {0.75, 0.9}}};
    for (const auto& point : points) {
        const WorldPoint world = projector.ProjectNormalized(point[0], point[1]);
        ASSERT_TRUE(world.IsFinite());
        EXPECT_NEAR(world.x, 100.0 * point[0], 1e-3);
        EXPECT_NEAR(world.y, 100.0 - 100.0 * point[1], 1e-3);

        auto back = projector.UnprojectNormalized(EnuPoint{world.x, world.y});
        ASSERT_TRUE(back) << back.status().ToString();
        EXPECT_NEAR(back.value()[0], point[0], 1e-5);
        EXPECT_NEAR(back.value()[1], point[1], 1e-5);
    }
}

TEST(BeVProjector, PixelAndNormalizedCalibrationsAgree) {
    auto normalized = HomographyProjector::Create(MakeNormalizedCalibration());
    ASSERT_TRUE(normalized) << normalized.status().ToString();

    CameraCalibration pixel = MakeNormalizedCalibration();
    pixel.homography_normalized = false;
    pixel.homography = Homography{100.0 / 1920.0, 0.0, 0.0, 0.0, -100.0 / 1080.0, 100.0, 0.0, 0.0, 1.0};
    auto pixelProjector = HomographyProjector::Create(pixel);
    ASSERT_TRUE(pixelProjector) << pixelProjector.status().ToString();
    EXPECT_FALSE(pixelProjector.value().IsNormalized());

    const ImageSize frame{1920, 1080};
    const WorldPoint fromNormalized = normalized.value().ProjectPixel(960.0, 540.0, frame);
    const WorldPoint fromPixel = pixelProjector.value().ProjectPixel(960.0, 540.0, frame);
    ASSERT_TRUE(fromNormalized.IsFinite());
    ASSERT_TRUE(fromPixel.IsFinite());
    EXPECT_NEAR(fromNormalized.x, 50.0, 1e-3);
    EXPECT_NEAR(fromNormalized.y, 50.0, 1e-3);
    EXPECT_NEAR(fromPixel.x, fromNormalized.x, 1e-3);
    EXPECT_NEAR(fromPixel.y, fromNormalized.y, 1e-3);

    auto back = pixelProjector.value().UnprojectPixel(EnuPoint{50.0, 50.0}, frame);
    ASSERT_TRUE(back) << back.status().ToString();
    EXPECT_NEAR(back.value()[0], 960.0, 1e-3);
    EXPECT_NEAR(back.value()[1], 540.0, 1e-3);
}

TEST(BeVProjector, RejectsPointsOnTheHorizon) {
    CameraCalibration calibration = MakeNormalizedCalibration();
    calibration.homography = Homography{100.0, 0.0, 0.0, 0.0, -100.0, 100.0, 1.0, 0.0, -1.0};
    auto created = HomographyProjector::Create(calibration);
    ASSERT_TRUE(created) << created.status().ToString();

    EXPECT_FALSE(created.value().ProjectNormalized(1.0, 0.5).IsFinite());
    EXPECT_TRUE(created.value().ProjectNormalized(0.5, 0.5).IsFinite());
}

TEST(BeVProjector, RejectsCalibrationWithoutHomography) {
    CameraCalibration calibration = MakeNormalizedCalibration();
    calibration.has_homography = false;
    auto created = HomographyProjector::Create(calibration);
    EXPECT_FALSE(created);
    EXPECT_EQ(created.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Homography solver
// ---------------------------------------------------------------------------

std::vector<Correspondence> ProjectPoints(const Homography& h,
                                          const std::vector<Correspondence>& image) {
    std::vector<Correspondence> ground;
    ground.reserve(image.size());
    for (const Correspondence& point : image) {
        const double denominator = h[6] * point[0] + h[7] * point[1] + h[8];
        ground.push_back({(h[0] * point[0] + h[1] * point[1] + h[2]) / denominator,
                          (h[3] * point[0] + h[4] * point[1] + h[5]) / denominator});
    }
    return ground;
}

TEST(BeVSolver, RecoversKnownHomographyWithDlt) {
    const Homography expected{2.0, 0.1, 10.0, 0.05, -2.0, 500.0, 1e-4, 1e-5, 1.0};
    const std::vector<Correspondence> image{{0.0, 0.0},    {1920.0, 0.0},  {1920.0, 1080.0},
                                            {0.0, 1080.0}, {640.0, 360.0}, {1280.0, 720.0}};
    const std::vector<Correspondence> ground = ProjectPoints(expected, image);

    auto fit = HomographySolver::Fit(image, ground, false);
    ASSERT_TRUE(fit) << fit.status().ToString();
    EXPECT_LT(fit.value().rms_error_m, 1e-4);
    EXPECT_EQ(fit.value().inliers, image.size());

    const std::vector<Correspondence> recovered = ProjectPoints(fit.value().homography, image);
    for (std::size_t i = 0; i < ground.size(); ++i) {
        EXPECT_NEAR(recovered[i][0], ground[i][0], 1e-4);
        EXPECT_NEAR(recovered[i][1], ground[i][1], 1e-4);
    }
}

TEST(BeVSolver, RansacRejectsAnOutlier) {
    const Homography expected{1.5, 0.0, 0.0, 0.0, -1.5, 900.0, 0.0, 0.0, 1.0};
    const std::vector<Correspondence> image{{0.0, 0.0},    {1920.0, 0.0},  {1920.0, 1080.0},
                                            {0.0, 1080.0}, {480.0, 270.0}, {960.0, 540.0},
                                            {1440.0, 810.0}, {240.0, 810.0}};
    std::vector<Correspondence> ground = ProjectPoints(expected, image);
    ground[3][0] += 1000.0;

    auto fit = HomographySolver::Fit(image, ground, true, 0.5);
    ASSERT_TRUE(fit) << fit.status().ToString();
    EXPECT_EQ(fit.value().inliers, image.size() - 1);
    EXPECT_LT(fit.value().rms_error_m, 1e-3);
}

TEST(BeVSolver, RejectsTooFewCorrespondences) {
    const std::vector<Correspondence> image{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}};
    const std::vector<Correspondence> ground = image;
    auto fit = HomographySolver::Fit(image, ground, false);
    EXPECT_FALSE(fit);
    EXPECT_EQ(fit.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Calibration store
// ---------------------------------------------------------------------------

TEST(CalibrationStore, LoadsYamlAndExposesSnapshots) {
    const TempFile file(kCalibrationYaml);
    auto loaded = CameraCalibrationStore::LoadYaml(file.Path());
    ASSERT_TRUE(loaded) << loaded.status().ToString();
    CameraCalibrationStore store = std::move(loaded).value();

    ASSERT_EQ(store.Size(), 1u);
    EXPECT_EQ(store.Generation(), 1u);
    EXPECT_EQ(store.ReloadCount(), 0u);
    EXPECT_EQ(store.Path(), file.Path());

    const std::vector<CameraId> cameras = store.Cameras();
    ASSERT_EQ(cameras.size(), 1u);
    EXPECT_EQ(cameras[0], "cam-01");

    const auto calibration = store.Get("cam-01");
    ASSERT_NE(calibration, nullptr);
    EXPECT_TRUE(calibration->has_homography);
    EXPECT_TRUE(calibration->homography_normalized);
    EXPECT_EQ(calibration->image_width, 1920);
    EXPECT_EQ(calibration->image_height, 1080);
    EXPECT_NEAR(calibration->origin_latitude, 32.606456, 1e-9);
    EXPECT_NEAR(calibration->origin_longitude, -85.481891, 1e-9);
    EXPECT_EQ(calibration->version, 1u);
    EXPECT_EQ(store.Get("cam-02"), nullptr);

    EXPECT_EQ(store.Snapshot().size(), 1u);
}

TEST(CalibrationStore, LoadsMultipleCalibrations) {
    const TempFile file(R"yaml(
calibrations:
  - camera_id: cam-01
    homography: [100, 0, 0, 0, -100, 100, 0, 0, 1]
    image_width: 1920
    image_height: 1080
    homography_normalized: true
  - camera_id: cam-02
    homography: [100, 0, 0, 0, -100, 100, 0, 0, 1]
    image_width: 1920
    image_height: 1080
    homography_normalized: true
)yaml");
    auto loaded = CameraCalibrationStore::LoadYaml(file.Path());
    ASSERT_TRUE(loaded) << loaded.status().ToString();
    EXPECT_EQ(loaded.value().Size(), 2u);
    EXPECT_NE(loaded.value().Get("cam-01"), nullptr);
    EXPECT_NE(loaded.value().Get("cam-02"), nullptr);
}

TEST(CalibrationStore, ReloadsChangedFileAndKeepsSnapshotOnError) {
    TempFile file(kCalibrationYaml);
    auto loaded = CameraCalibrationStore::LoadYaml(file.Path());
    ASSERT_TRUE(loaded) << loaded.status().ToString();
    CameraCalibrationStore store = std::move(loaded).value();

    // Unchanged file: no reload.
    ASSERT_TRUE(store.ReloadIfChanged().ok());
    EXPECT_EQ(store.ReloadCount(), 0u);
    EXPECT_EQ(store.Get("cam-01")->version, 1u);

    file.Write(kCalibrationYamlV2);
    ASSERT_TRUE(store.ReloadIfChanged().ok());
    EXPECT_EQ(store.ReloadCount(), 1u);
    EXPECT_EQ(store.Get("cam-01")->version, 2u);
    EXPECT_EQ(store.Get("cam-01")->source, "estimated");

    // Broken file: the previous snapshot survives.
    file.Write("calibrations:\n  - camera_id: cam-01\n    homography: [1, 0, 0, 0, 1, 0, 0, 0, 0]\n");
    EXPECT_FALSE(store.ReloadIfChanged().ok());
    EXPECT_EQ(store.Get("cam-01")->version, 2u);
    EXPECT_EQ(store.ReloadCount(), 1u);
}

TEST(CalibrationStore, RejectsOlderVersionsOnUpsert) {
    auto created = CameraCalibrationStore::FromCalibrations({MakeNormalizedCalibration()});
    ASSERT_TRUE(created) << created.status().ToString();
    CameraCalibrationStore store = std::move(created).value();

    CameraCalibration newer = MakeNormalizedCalibration();
    newer.version = 2;
    ASSERT_TRUE(store.Upsert(newer).ok());

    CameraCalibration older = MakeNormalizedCalibration();
    older.version = 1;
    const tmcmt::Status status = store.Upsert(older);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), tmcmt::ErrorCode::InvalidArgument);
    EXPECT_EQ(store.Get("cam-01")->version, 2u);
}

TEST(CalibrationStore, ValidatesCalibrations) {
    CameraCalibrationStore store;

    CameraCalibration pixel = MakeNormalizedCalibration();
    pixel.homography_normalized = false;
    pixel.image_width = 0;
    pixel.image_height = 0;
    EXPECT_FALSE(store.Upsert(pixel).ok());

    CameraCalibration degenerate = MakeNormalizedCalibration();
    degenerate.homography[8] = 0.0;
    EXPECT_FALSE(store.Upsert(degenerate).ok());

    CameraCalibration emptyCamera = MakeNormalizedCalibration();
    emptyCamera.camera_id.clear();
    EXPECT_FALSE(store.Upsert(emptyCamera).ok());

    CalibrationStoreOptions strict;
    strict.require_homography = true;
    CameraCalibration noHomography = MakeNormalizedCalibration();
    noHomography.has_homography = false;
    auto created = CameraCalibrationStore::FromCalibrations({noHomography}, strict);
    EXPECT_FALSE(created);
    EXPECT_EQ(created.status().code(), tmcmt::ErrorCode::InvalidArgument);

    CalibrationStoreOptions errorLimit;
    errorLimit.max_reprojection_error_m = 0.5;
    CameraCalibration noisy = MakeNormalizedCalibration();
    noisy.reprojection_error_m = 2.0f;
    auto rejected = CameraCalibrationStore::FromCalibrations({noisy}, errorLimit);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

TEST(CalibrationStore, RejectsDuplicateCameraIds) {
    auto created = CameraCalibrationStore::FromCalibrations(
        {MakeNormalizedCalibration("cam-01"), MakeNormalizedCalibration("cam-01")});
    EXPECT_FALSE(created);
    EXPECT_EQ(created.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

TEST(CalibrationStore, ReportsMissingFile) {
    auto loaded = CameraCalibrationStore::LoadYaml("definitely_missing_calibration.yaml");
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.status().code(), tmcmt::ErrorCode::IoError);
}

// ---------------------------------------------------------------------------
// World trajectory fusion
// ---------------------------------------------------------------------------

TEST(WorldFusion, ProjectsMissingWorldFromBoundingBoxes) {
    WorldFusionOptions options;
    options.enabled = true;
    WorldTrajectoryFusion fusion(options);

    Tracklet tracklet = MakeWorldSegment("cam-01", 1, ObjectType::Car, {{1000, EnuPoint{0.0, 0.0}}});
    tracklet.observations[0].bbox = BBox{0.f, 0.f, 1920.f, 1080.f, 0.9f};
    tracklet.observations[0].world = WorldPoint{};

    const CameraCalibration calibration = MakeNormalizedCalibration();
    const tmcmt::Status status = fusion.Normalize(tracklet, &calibration);
    ASSERT_TRUE(status.ok()) << status.ToString();

    ASSERT_TRUE(tracklet.observations[0].world.IsFinite());
    EXPECT_NEAR(tracklet.observations[0].world.x, 50.0f, 1e-3f);
    EXPECT_NEAR(tracklet.observations[0].world.y, 0.0f, 1e-3f);

    const tmcmt::bev::FusionStats stats = fusion.Stats();
    EXPECT_EQ(stats.observations, 1u);
    EXPECT_EQ(stats.projected, 1u);
    EXPECT_EQ(stats.projection_failures, 0u);
    EXPECT_EQ(stats.missing_world, 0u);
}

TEST(WorldFusion, OverwriteWorldReprojectsExistingPoints) {
    WorldFusionOptions options;
    options.enabled = true;
    options.overwrite_world = true;
    WorldTrajectoryFusion fusion(options);

    Tracklet tracklet = MakeWorldSegment("cam-01", 1, ObjectType::Car, {{1000, EnuPoint{1.0, 2.0}}});
    tracklet.observations[0].bbox = BBox{0.f, 0.f, 1920.f, 1080.f, 0.9f};

    const CameraCalibration calibration = MakeNormalizedCalibration();
    ASSERT_TRUE(fusion.Normalize(tracklet, &calibration).ok());
    ASSERT_TRUE(tracklet.observations[0].world.IsFinite());
    EXPECT_NEAR(tracklet.observations[0].world.x, 50.0f, 1e-3f);
    EXPECT_NEAR(tracklet.observations[0].world.y, 0.0f, 1e-3f);
    EXPECT_EQ(fusion.Stats().projected, 1u);
}

TEST(WorldFusion, SuppressesOverlapDuplicates) {
    WorldFusionOptions options;
    options.enabled = true;
    options.duplicate_distance_m = 1.5;
    options.duplicate_window_ms = 500;
    WorldTrajectoryFusion fusion(options);

    GlobalTrackUpdate update;
    update.global_id = 7;
    update.type = ObjectType::Car;
    update.segments.push_back(
        MakeWorldSegment("cam-01", 1, ObjectType::Car, {{1000, EnuPoint{10.0, 20.0}}}, 0.9f));
    update.segments.push_back(
        MakeWorldSegment("cam-02", 2, ObjectType::Car, {{1100, EnuPoint{10.5, 20.2}}}, 0.8f));

    const WorldTrajectory trajectory = fusion.Fuse(update);
    ASSERT_EQ(trajectory.samples.size(), 1u);
    EXPECT_EQ(trajectory.duplicates_suppressed, 1u);
    EXPECT_EQ(trajectory.samples[0].camera, "cam-01");
    EXPECT_EQ(fusion.Stats().duplicates_suppressed, 1u);
    EXPECT_EQ(fusion.Stats().overlap_pairs, 1u);
    EXPECT_EQ(fusion.Stats().samples_in, 2u);
    EXPECT_EQ(fusion.Stats().samples_out, 1u);
}

TEST(WorldFusion, KeepsSameCameraObservationsAndDistantPairs) {
    WorldFusionOptions options;
    options.enabled = true;
    options.duplicate_distance_m = 1.5;
    options.duplicate_window_ms = 500;
    WorldTrajectoryFusion fusion(options);

    GlobalTrackUpdate update;
    update.global_id = 8;
    update.type = ObjectType::Car;
    update.segments.push_back(
        MakeWorldSegment("cam-01", 1, ObjectType::Car, {{1000, EnuPoint{10.0, 20.0}}}));
    update.segments.push_back(
        MakeWorldSegment("cam-01", 2, ObjectType::Car, {{1000, EnuPoint{10.0, 20.0}}}));
    update.segments.push_back(
        MakeWorldSegment("cam-02", 3, ObjectType::Car, {{1000, EnuPoint{50.0, 50.0}}}));

    const WorldTrajectory trajectory = fusion.Fuse(update);
    EXPECT_EQ(trajectory.samples.size(), 3u);
    EXPECT_EQ(trajectory.duplicates_suppressed, 0u);
}

TEST(WorldFusion, EstimatesSpeedAndCourse) {
    WorldFusionOptions options;
    options.enabled = true;
    options.speed_window_ms = 1000;
    WorldTrajectoryFusion fusion(options);

    GlobalTrackUpdate update;
    update.global_id = 9;
    update.type = ObjectType::Car;
    update.segments.push_back(MakeWorldSegment(
        "cam-01", 1, ObjectType::Car,
        {{0, EnuPoint{0.0, 0.0}}, {1000, EnuPoint{10.0, 0.0}}, {2000, EnuPoint{20.0, 0.0}}}));

    const WorldTrajectory trajectory = fusion.Fuse(update);
    ASSERT_EQ(trajectory.samples.size(), 3u);
    EXPECT_TRUE(trajectory.has_velocity);
    EXPECT_NEAR(trajectory.speed_mps, 10.0, 0.5);
    EXPECT_NEAR(trajectory.course_deg, 90.0, 1.0);
    EXPECT_EQ(trajectory.begin, Timestamp(0));
    EXPECT_EQ(trajectory.end, Timestamp(2000));
}

TEST(WorldFusion, IsDeterministicForShuffledSegments) {
    WorldFusionOptions options;
    options.enabled = true;
    WorldTrajectoryFusion fusion(options);

    Tracklet first = MakeWorldSegment("cam-01", 1, ObjectType::Car, {{1000, EnuPoint{0.0, 0.0}}});
    Tracklet second = MakeWorldSegment("cam-02", 2, ObjectType::Car, {{1000, EnuPoint{100.0, 0.0}}});

    GlobalTrackUpdate forward;
    forward.global_id = 10;
    forward.type = ObjectType::Car;
    forward.segments = {first, second};

    GlobalTrackUpdate backward;
    backward.global_id = 10;
    backward.type = ObjectType::Car;
    backward.segments = {second, first};

    const WorldTrajectory a = fusion.Fuse(forward);
    const WorldTrajectory b = fusion.Fuse(backward);
    ASSERT_EQ(a.samples.size(), b.samples.size());
    for (std::size_t i = 0; i < a.samples.size(); ++i) {
        EXPECT_EQ(a.samples[i].camera, b.samples[i].camera);
        EXPECT_EQ(a.samples[i].tracklet_id, b.samples[i].tracklet_id);
        EXPECT_DOUBLE_EQ(a.samples[i].position.east, b.samples[i].position.east);
        EXPECT_DOUBLE_EQ(a.samples[i].position.north, b.samples[i].position.north);
    }
}

// ---------------------------------------------------------------------------
// World track sink
// ---------------------------------------------------------------------------

TEST(WorldTrackSink, WritesGeoJsonFeatures) {
    const TempFile file("", ".geojsonl");
    auto opened = WorldTrackSink::Open(file.Path(), WorldOutputFormat::GeoJson);
    ASSERT_TRUE(opened) << opened.status().ToString();
    const auto sink = std::move(opened).value();
    EXPECT_EQ(sink->format(), WorldOutputFormat::GeoJson);

    GlobalTrackUpdate update;
    update.global_id = 42;
    update.type = ObjectType::Car;
    Tracklet segment = MakeWorldSegment("cam-01", 1, ObjectType::Car, {{1000, EnuPoint{10.0, 20.0}}});
    segment.origin_latitude = 32.606456;
    segment.origin_longitude = -85.481891;
    update.segments.push_back(std::move(segment));

    ASSERT_TRUE(sink->Publish(update).ok());
    ASSERT_TRUE(sink->Flush().ok());
    EXPECT_EQ(sink->records(), 1u);
    EXPECT_EQ(sink->trajectories(), 1u);

    const std::string content = ReadFile(file.Path());
    EXPECT_NE(content.find("\"type\":\"Feature\""), std::string::npos);
    EXPECT_NE(content.find("\"global_id\":42"), std::string::npos);
    EXPECT_NE(content.find("\"camera\":\"cam-01\""), std::string::npos);
    EXPECT_NE(content.find("\"frame\":\"wgs84\""), std::string::npos);
    EXPECT_NE(content.find("\"speed_mps\""), std::string::npos);
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 1);
}

TEST(WorldTrackSink, WritesCsvWithHeader) {
    const TempFile file("", ".csv");
    auto opened = WorldTrackSink::Open(file.Path(), WorldOutputFormat::Csv);
    ASSERT_TRUE(opened) << opened.status().ToString();
    const auto sink = std::move(opened).value();

    GlobalTrackUpdate update;
    update.global_id = 42;
    update.type = ObjectType::Car;
    update.segments.push_back(
        MakeWorldSegment("cam-01", 1, ObjectType::Car, {{1000, EnuPoint{10.0, 20.0}}}));

    ASSERT_TRUE(sink->Publish(update).ok());
    ASSERT_TRUE(sink->Flush().ok());
    EXPECT_EQ(sink->records(), 1u);

    const std::string content = ReadFile(file.Path());
    EXPECT_NE(content.find("global_id,ts_ms,tracklet_id,camera,type"), std::string::npos);
    EXPECT_NE(content.find("\n42,1000,"), std::string::npos);
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 2);
}

TEST(WorldTrackSink, RejectsEmptyPathAndDisabledFormat) {
    EXPECT_FALSE(WorldTrackSink::Open("", WorldOutputFormat::GeoJson));
    EXPECT_FALSE(WorldTrackSink::Open("world.geojsonl", WorldOutputFormat::Off));
}

TEST(WorldOutputFormat, ParsesNames) {
    using tmcmt::bev::ParseWorldOutputFormat;
    EXPECT_EQ(ParseWorldOutputFormat("geojson"), WorldOutputFormat::GeoJson);
    EXPECT_EQ(ParseWorldOutputFormat("GeoJSONL"), WorldOutputFormat::GeoJson);
    EXPECT_EQ(ParseWorldOutputFormat("csv"), WorldOutputFormat::Csv);
    EXPECT_EQ(ParseWorldOutputFormat("CSV"), WorldOutputFormat::Csv);
    EXPECT_EQ(ParseWorldOutputFormat("shapefile"), WorldOutputFormat::Off);
}

// ---------------------------------------------------------------------------
// BEV spatial gate
// ---------------------------------------------------------------------------

TEST(BeVSpatialGate, FallsBackToZonesWhenWorldEndpointsArePartial) {
    auto model = CameraLinkModel::LoadFromString(R"yaml(
default_linked: false
fallback:
  min_transition_ms: 0
  max_transition_ms: 120000
cameras:
  cam-1:
    exit_zone: [[0,0],[10,0],[10,10],[0,10]]
  cam-2:
    entry_zone: [[100,0],[110,0],[110,10],[100,10]]
links:
  - from: cam-1
    to: cam-2
)yaml");
    ASSERT_TRUE(model) << model.status().ToString();

    SpatialGateOptions options;
    options.mode = GateMode::Bev;
    options.window = TransitionWindow{0, 120000};
    options.max_distance_m = 50.0;
    options.max_speed_mps = 30.0;

    // Only the exit of the first camera and the entry of the second are known.
    const TrackletEndpoint from = MakeEndpoint("cam-1", 0, 1000, std::nullopt, EnuPoint{5.0, 5.0});
    const TrackletEndpoint to = MakeEndpoint("cam-2", 2000, 3000, EnuPoint{105.0, 5.0}, std::nullopt);

    const BeVSpatialGate fallback(&model.value(), options, true);
    EXPECT_TRUE(fallback.FallsBackToZone());
    EXPECT_EQ(fallback.Preferred().Options().mode, GateMode::Bev);
    EXPECT_EQ(fallback.Zone().Options().mode, GateMode::Zone);
    const tmcmt::st::GateDecision allowed = fallback.Check(from, to);
    EXPECT_TRUE(allowed.allowed);
    EXPECT_EQ(allowed.reason, GateReason::Allowed);

    // Without the fallback the pair is measured in BEV: 100 m apart in 1 s.
    const BeVSpatialGate strict(&model.value(), options, false);
    EXPECT_FALSE(strict.FallsBackToZone());
    const tmcmt::st::GateDecision denied = strict.Check(from, to);
    EXPECT_FALSE(denied.allowed);
    EXPECT_EQ(denied.reason, GateReason::Distance);
}

TEST(BeVSpatialGate, RejectsDistantPairWithFullWorld) {
    SpatialGateOptions options;
    options.mode = GateMode::Bev;
    options.window = TransitionWindow{0, 120000};
    options.max_distance_m = 50.0;
    options.max_speed_mps = 30.0;

    const TrackletEndpoint from = MakeEndpoint("cam-1", 0, 1000, EnuPoint{0.0, 0.0}, EnuPoint{5.0, 5.0});
    const TrackletEndpoint to =
        MakeEndpoint("cam-2", 2000, 3000, EnuPoint{105.0, 5.0}, EnuPoint{110.0, 5.0});

    const BeVSpatialGate gate(nullptr, options, true);
    const tmcmt::st::GateDecision decision = gate.Check(from, to);
    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.reason, GateReason::Distance);
}

TEST(BeVSpatialGate, ReportsMissingWorld) {
    SpatialGateOptions options;
    options.mode = GateMode::Bev;
    options.window = TransitionWindow{0, 120000};

    const TrackletEndpoint from = MakeEndpoint("cam-1", 0, 1000, std::nullopt, std::nullopt);
    const TrackletEndpoint to = MakeEndpoint("cam-2", 2000, 3000, std::nullopt, std::nullopt);

    const BeVSpatialGate gate(nullptr, options, true);
    const tmcmt::st::GateDecision decision = gate.Check(from, to);
    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.reason, GateReason::MissingWorld);

    SpatialGateOptions permissive = options;
    permissive.allow_missing_world = true;
    const BeVSpatialGate permissiveGate(nullptr, permissive, true);
    EXPECT_TRUE(permissiveGate.Check(from, to).allowed);
}

} // namespace
