#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/map/AccelerationEstimator.h"
#include "trafficmcmt/map/MapConfig.h"
#include "trafficmcmt/map/MapFeed.h"
#include "trafficmcmt/map/MapIdRegistry.h"
#include "trafficmcmt/map/MapTypes.h"
#include "trafficmcmt/map/TrackletTimeline.h"
#include "trafficmcmt/map/TrailBuilder.h"
#include "trafficmcmt/map/VelocityEstimator.h"

namespace {

using tmcmt::EnuPoint;
using tmcmt::GeoPoint;
using tmcmt::GeographicToEnu;
using tmcmt::ObjectType;
using tmcmt::Observation;
using tmcmt::Timestamp;
using tmcmt::Tracklet;

namespace map = tmcmt::map;

constexpr double kOriginLat = 32.606456;
constexpr double kOriginLon = -85.481891;

Tracklet MakeTracklet(std::string camera, std::uint64_t localId, ObjectType type,
                      const std::vector<Timestamp>& stamps, double metresPerSecond) {
    Tracklet tracklet;
    tracklet.session_id = "session";
    tracklet.camera = std::move(camera);
    tracklet.local_track_id = localId;
    tracklet.type = type;
    tracklet.begin = stamps.front();
    tracklet.end = stamps.back();

    for (const Timestamp ts : stamps) {
        Observation observation;
        observation.ts = ts;
        observation.bbox = tmcmt::BBox{0.f, 0.f, 10.f, 10.f, 1.f};
        observation.world = tmcmt::WorldPoint{
            static_cast<float>(metresPerSecond * static_cast<double>(ts.count()) / 1000.0), 0.f, true};
        tracklet.observations.push_back(observation);
    }
    return tracklet;
}

map::MapFeedConfig MakeFeedConfig() {
    map::MapFeedConfig config;
    config.has_origin = true;
    config.origin_latitude = kOriginLat;
    config.origin_longitude = kOriginLon;
    config.tick_hz = 10;
    config.trail_points = 40;
    config.trail_min_distance_m = 0.3;
    config.velocity_window_s = 1.0;
    config.velocity_max_mps = 70.0;
    config.track_ttl_s = 1.0;
    config.heartbeat_s = 5.0;

    map::MapCameraConfig first;
    first.id = "cam-01";
    first.camera_id = 1;
    first.polygon = {GeoPoint{32.60, -85.49}, GeoPoint{32.61, -85.49}, GeoPoint{32.61, -85.48}};
    config.cameras.push_back(first);

    map::MapCameraConfig second;
    second.id = "cam-02";
    second.camera_id = 2;
    config.cameras.push_back(second);
    return config;
}

class TempFile {
public:
    explicit TempFile(std::string name, std::string contents) {
        m_path = std::filesystem::temp_directory_path() / std::move(name);
        std::ofstream stream(m_path);
        stream << contents;
    }

    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    [[nodiscard]] std::string Path() const {
        return m_path.string();
    }

private:
    std::filesystem::path m_path;
};

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

TEST(MapIdRegistry, AssignsSequentialStableIds) {
    map::MapIdRegistry registry;
    const map::TrackKey a{"session", "cam-01", 7};
    const map::TrackKey b{"session", "cam-02", 7};

    EXPECT_EQ(registry.Assign(a), 1U);
    EXPECT_EQ(registry.Assign(a), 1U);
    EXPECT_EQ(registry.Assign(b), 2U);
    EXPECT_EQ(registry.Size(), 2U);
    EXPECT_EQ(registry.NextId(), 3U);

    ASSERT_TRUE(registry.Find(a).has_value());
    EXPECT_EQ(registry.Find(a).value(), 1U);
}

TEST(MapIdRegistry, RenameRejectsTakenAndUnknownIds) {
    map::MapIdRegistry registry;
    const map::TrackKey a{"session", "cam-01", 1};
    const map::TrackKey b{"session", "cam-02", 2};
    EXPECT_EQ(registry.Assign(a), 1U);
    EXPECT_EQ(registry.Assign(b), 2U);

    EXPECT_FALSE(registry.Rename(1, 1));
    EXPECT_FALSE(registry.Rename(1, 2));
    EXPECT_FALSE(registry.Rename(99, 3));
    EXPECT_TRUE(registry.Rename(2, 10));
    EXPECT_EQ(registry.Find(b).value(), 10U);
    EXPECT_EQ(registry.NextId(), 11U);
}

TEST(VelocityEstimator, LeastSquaresConstantVelocity) {
    map::VelocityEstimator estimator(1.0, 70.0);
    for (int i = 0; i <= 10; ++i) {
        const Timestamp ts(i * 100);
        estimator.Add(ts, EnuPoint{0.01 * static_cast<double>(ts.count()), 0.0});
    }
    const auto velocity = estimator.Estimate(Timestamp(1000));
    ASSERT_TRUE(velocity.has_value());
    EXPECT_NEAR(velocity->east, 10.0, 1e-6);
    EXPECT_NEAR(velocity->north, 0.0, 1e-6);
}

TEST(VelocityEstimator, ReturnsNulloptWhenStaleOrShort) {
    map::VelocityEstimator estimator(1.0, 70.0);
    EXPECT_FALSE(estimator.Estimate(Timestamp(0)).has_value());

    estimator.Add(Timestamp(0), EnuPoint{0.0, 0.0});
    EXPECT_FALSE(estimator.Estimate(Timestamp(0)).has_value());

    estimator.Add(Timestamp(100), EnuPoint{1.0, 0.0});
    EXPECT_TRUE(estimator.Estimate(Timestamp(100)).has_value());
    EXPECT_FALSE(estimator.Estimate(Timestamp(2500)).has_value());
}

TEST(VelocityEstimator, ClampsUnrealisticSpeed) {
    map::VelocityEstimator estimator(1.0, 70.0);
    estimator.Add(Timestamp(0), EnuPoint{0.0, 0.0});
    estimator.Add(Timestamp(100), EnuPoint{100.0, 0.0});
    const auto velocity = estimator.Estimate(Timestamp(100));
    ASSERT_TRUE(velocity.has_value());
    EXPECT_NEAR(std::hypot(velocity->east, velocity->north), 70.0, 1e-6);
}

TEST(TrailBuilder, DecimatesAndCapsPoints) {
    map::TrailBuilder trail(3, 0.5);
    trail.Add(EnuPoint{0.0, 0.0});
    trail.Add(EnuPoint{0.1, 0.0});
    trail.Add(EnuPoint{0.6, 0.0});
    trail.Add(EnuPoint{1.2, 0.0});
    trail.Add(EnuPoint{1.8, 0.0});

    ASSERT_EQ(trail.Points().size(), 3U);
    EXPECT_NEAR(trail.Points()[0].east, 0.6, 1e-9);
    EXPECT_NEAR(trail.Points()[2].east, 1.8, 1e-9);
}

TEST(TrackletTimeline, InterleavesChunksByTimestamp) {
    map::TrackletTimeline timeline;
    timeline.Add(MakeTracklet("cam-01", 1, ObjectType::Car,
                              {Timestamp(100), Timestamp(300), Timestamp(500)}, 1.0));
    timeline.Add(MakeTracklet("cam-02", 2, ObjectType::Pedestrian,
                              {Timestamp(200), Timestamp(400)}, 1.0));

    EXPECT_EQ(timeline.TrackletCount(), 2U);
    EXPECT_EQ(timeline.Begin().value(), Timestamp(100));
    EXPECT_EQ(timeline.End().value(), Timestamp(500));

    const std::int64_t expected[] = {100, 200, 300, 400, 500};
    map::ObservationEvent event;
    for (const std::int64_t stamp : expected) {
        ASSERT_TRUE(timeline.Next(event));
        EXPECT_EQ(event.observation->ts, Timestamp(stamp));
        ASSERT_NE(event.tracklet, nullptr);
    }
    EXPECT_TRUE(timeline.Empty());
}

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

TEST(MapProtocol, SerialisesTracksUpdate) {
    map::MapFrame frame;
    frame.type = map::MapFrameType::TracksUpdate;
    frame.timestamp = Timestamp(1234);
    frame.snapshot = false;

    map::MapTrackUpdate track;
    track.global_id = 7;
    track.camera_id = 2;
    track.class_name = "car";
    track.position = GeoPoint{45.5266, 5.9745};
    track.velocity = EnuPoint{1.0, -2.0};
    track.has_velocity = true;
    track.speed_kmh = 8.05;
    track.trail = {GeoPoint{45.5266, 5.9745}, GeoPoint{45.5267, 5.9746}};
    frame.tracks.push_back(track);

    const std::string json = map::ToJson(frame);
    EXPECT_NE(json.find("\"type\":\"tracks_update\""), std::string::npos);
    EXPECT_NE(json.find("\"timestamp\":1234"), std::string::npos);
    EXPECT_NE(json.find("\"mode\":\"delta\""), std::string::npos);
    EXPECT_NE(json.find("\"global_id\":7"), std::string::npos);
    EXPECT_NE(json.find("\"camera_id\":2"), std::string::npos);
    EXPECT_NE(json.find("\"class\":\"car\""), std::string::npos);
    EXPECT_NE(json.find("\"position\":[5.9745000,45.5266000]"), std::string::npos);
    EXPECT_NE(json.find("\"velocity\":[1.00,-2.00]"), std::string::npos);
    EXPECT_NE(json.find("\"speed_kmh\":8.05"), std::string::npos);
    EXPECT_NE(json.find("\"trail\":[[5.9745000,45.5266000],[5.9746000,45.5267000]]"), std::string::npos);
}

TEST(MapProtocol, SerialisesAdditiveFrames) {
    map::MapFrame remove;
    remove.type = map::MapFrameType::TracksRemove;
    remove.timestamp = Timestamp(10);
    remove.removed_ids = {1, 2};
    EXPECT_EQ(map::ToJson(remove), "{\"type\":\"tracks_remove\",\"timestamp\":10,\"ids\":[1,2]}");

    map::MapFrame merge;
    merge.type = map::MapFrameType::IdMerge;
    merge.timestamp = Timestamp(11);
    merge.old_id = 3;
    merge.new_id = 9;
    EXPECT_EQ(map::ToJson(merge), "{\"type\":\"id_merge\",\"timestamp\":11,\"old_id\":3,\"new_id\":9}");

    map::MapFrame heartbeat;
    heartbeat.type = map::MapFrameType::Heartbeat;
    heartbeat.timestamp = Timestamp(12);
    EXPECT_EQ(map::ToJson(heartbeat), "{\"type\":\"heartbeat\",\"timestamp\":12}");
}

TEST(MapProtocol, MapsObjectTypesToViewerClasses) {
    EXPECT_STREQ(map::MapClassName(ObjectType::Pedestrian), "person");
    EXPECT_STREQ(map::MapClassName(ObjectType::Car), "car");
    EXPECT_STREQ(map::MapClassName(ObjectType::Truck), "truck");
    EXPECT_STREQ(map::MapClassName(ObjectType::Bus), "bus");
    EXPECT_STREQ(map::MapClassName(ObjectType::Bike), "bike");
    EXPECT_STREQ(map::MapClassName(ObjectType::Unknown), "unknown");
}

// ---------------------------------------------------------------------------
// Feed
// ---------------------------------------------------------------------------

TEST(MapFeed, EmitsPositionVelocityAndTrail) {
    map::MapFeed feed(MakeFeedConfig());
    std::vector<Timestamp> stamps;
    for (int i = 0; i <= 10; ++i) {
        stamps.emplace_back(i * 100);
    }
    const Tracklet tracklet = MakeTracklet("cam-01", 7, ObjectType::Car, stamps, 10.0);
    for (const Observation& observation : tracklet.observations) {
        feed.Observe(tracklet, observation);
    }

    const auto frames = feed.Tick(Timestamp(1000));
    ASSERT_EQ(frames.size(), 1U);
    const map::MapFrame& update = frames.front();
    EXPECT_EQ(update.type, map::MapFrameType::TracksUpdate);
    EXPECT_FALSE(update.snapshot);
    ASSERT_EQ(update.tracks.size(), 1U);

    const map::MapTrackUpdate& track = update.tracks.front();
    EXPECT_EQ(track.global_id, 1U);
    EXPECT_EQ(track.camera_id, 1);
    EXPECT_EQ(track.class_name, "car");

    const EnuPoint position = GeographicToEnu(track.position, GeoPoint{kOriginLat, kOriginLon});
    EXPECT_NEAR(position.east, 10.0, 0.05);
    EXPECT_NEAR(position.north, 0.0, 0.05);

    ASSERT_TRUE(track.has_velocity);
    EXPECT_NEAR(track.velocity.east, 10.0, 0.1);
    EXPECT_NEAR(track.speed_kmh, 36.0, 0.5);
    EXPECT_EQ(track.trail.size(), 11U);

    // A second delta without new observations must stay empty.
    EXPECT_TRUE(feed.Tick(Timestamp(1100)).empty());
}

TEST(MapFeed, SnapshotContainsFullTrail) {
    map::MapFeed feed(MakeFeedConfig());
    std::vector<Timestamp> stamps;
    for (int i = 0; i <= 5; ++i) {
        stamps.emplace_back(i * 100);
    }
    const Tracklet tracklet = MakeTracklet("cam-01", 1, ObjectType::Car, stamps, 5.0);
    for (const Observation& observation : tracklet.observations) {
        feed.Observe(tracklet, observation);
    }

    const map::MapFrame snapshot = feed.Snapshot(Timestamp(500));
    EXPECT_TRUE(snapshot.snapshot);
    ASSERT_EQ(snapshot.tracks.size(), 1U);
    EXPECT_EQ(snapshot.tracks.front().trail.size(), 6U);
}

TEST(MapFeed, RemovesExpiredTracks) {
    map::MapFeed feed(MakeFeedConfig());
    const Tracklet tracklet =
        MakeTracklet("cam-01", 1, ObjectType::Car, {Timestamp(0), Timestamp(100)}, 1.0);
    for (const Observation& observation : tracklet.observations) {
        feed.Observe(tracklet, observation);
    }
    EXPECT_EQ(feed.ActiveTracks(), 1U);

    const auto frames = feed.Tick(Timestamp(2000));
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames.front().type, map::MapFrameType::TracksRemove);
    ASSERT_EQ(frames.front().removed_ids.size(), 1U);
    EXPECT_EQ(frames.front().removed_ids.front(), 1U);
    EXPECT_EQ(feed.ActiveTracks(), 0U);
}

TEST(MapFeed, SkipsUnknownCamerasAndInvalidWorldPoints) {
    map::MapFeed feed(MakeFeedConfig());

    const Tracklet unknown =
        MakeTracklet("cam-99", 1, ObjectType::Car, {Timestamp(0), Timestamp(100)}, 1.0);
    for (const Observation& observation : unknown.observations) {
        feed.Observe(unknown, observation);
    }
    EXPECT_EQ(feed.UnknownCameraObservations(), 2U);

    Tracklet invalid = MakeTracklet("cam-01", 2, ObjectType::Car, {Timestamp(0), Timestamp(100)}, 1.0);
    for (Observation& observation : invalid.observations) {
        observation.world.valid = false;
    }
    for (const Observation& observation : invalid.observations) {
        feed.Observe(invalid, observation);
    }
    EXPECT_EQ(feed.ActiveTracks(), 0U);
}

TEST(MapFeed, EmitsCameraZones) {
    map::MapFeed feed(MakeFeedConfig());
    const map::MapFrame zones = feed.CameraZones(Timestamp(0));
    EXPECT_EQ(zones.type, map::MapFrameType::CameraZones);
    ASSERT_EQ(zones.zones.size(), 1U);
    EXPECT_EQ(zones.zones.front().camera_id, 1);
    EXPECT_EQ(zones.zones.front().polygon.size(), 3U);
}

TEST(MapFeed, RenamesTrackAfterMerge) {
    map::MapFeed feed(MakeFeedConfig());
    const Tracklet tracklet =
        MakeTracklet("cam-01", 1, ObjectType::Car, {Timestamp(0), Timestamp(100)}, 1.0);
    for (const Observation& observation : tracklet.observations) {
        feed.Observe(tracklet, observation);
    }

    const auto frame = feed.Rename(1, 42);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->type, map::MapFrameType::IdMerge);
    EXPECT_EQ(frame->old_id, 1U);
    EXPECT_EQ(frame->new_id, 42U);

    const map::MapFrame snapshot = feed.Snapshot(Timestamp(100));
    ASSERT_EQ(snapshot.tracks.size(), 1U);
    EXPECT_EQ(snapshot.tracks.front().global_id, 42U);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST(MapConfig, LoadsOriginColorsAndGeoBindingPolygon) {
    const TempFile binding("tmcmt_map_binding.ini",
                           "[points]\n"
                           "lat0 = 32.606318\n"
                           "lon0 = -85.482046\n"
                           "lat1 = 32.606402\n"
                           "lon1 = -85.481739\n"
                           "lat2 = 32.606531\n"
                           "lon2 = -85.481845\n"
                           "lat3 = 32.606573\n"
                           "lon3 = -85.481934\n");
    const TempFile config("tmcmt_map_feed.yaml",
                          "feed:\n"
                          "  origin: 32.606456,-85.481891\n"
                          "  tick_hz: 5\n"
                          "  trail_points: 12\n"
                          "cameras:\n"
                          "  - id: cam-01\n"
                          "    camera_id: 3\n"
                          "    color: [1, 2, 3, 4]\n"
                          "    geo_bind: '" +
                              binding.Path() + "'\n");

    auto loaded = map::LoadMapFeedConfig(config.Path());
    ASSERT_TRUE(loaded) << loaded.status().ToString();
    const map::MapFeedConfig& value = loaded.value();
    EXPECT_TRUE(value.has_origin);
    EXPECT_DOUBLE_EQ(value.origin_latitude, kOriginLat);
    EXPECT_DOUBLE_EQ(value.origin_longitude, kOriginLon);
    EXPECT_EQ(value.tick_hz, 5);
    EXPECT_EQ(value.trail_points, 12U);
    ASSERT_EQ(value.cameras.size(), 1U);
    EXPECT_EQ(value.cameras.front().camera_id, 3);
    EXPECT_EQ(value.cameras.front().color[3], 4);
    EXPECT_EQ(value.cameras.front().polygon.size(), 4U);
}

TEST(MapConfig, RejectsDuplicateCameraIds) {
    const TempFile config("tmcmt_map_duplicate.yaml",
                          "feed:\n"
                          "  origin: 32.606456,-85.481891\n"
                          "cameras:\n"
                          "  - id: cam-01\n"
                          "    camera_id: 1\n"
                          "  - id: cam-02\n"
                          "    camera_id: 1\n");
    auto loaded = map::LoadMapFeedConfig(config.Path());
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

TEST(MapConfig, ResolvesOriginFromConfigOrStream) {
    map::MapFeedConfig config = MakeFeedConfig();
    Tracklet plain = MakeTracklet("cam-01", 1, ObjectType::Car, {Timestamp(0)}, 1.0);

    auto fromConfig = map::ResolveMapOrigin({plain}, config);
    ASSERT_TRUE(fromConfig);
    EXPECT_FALSE(fromConfig.value().from_stream);
    EXPECT_DOUBLE_EQ(fromConfig.value().origin.latitude, kOriginLat);

    config.has_origin = false;
    Tracklet withOrigin = plain;
    withOrigin.origin_latitude = 45.52724575;
    withOrigin.origin_longitude = 5.97433625;
    auto fromStream = map::ResolveMapOrigin({withOrigin}, config);
    ASSERT_TRUE(fromStream);
    EXPECT_TRUE(fromStream.value().from_stream);
    EXPECT_DOUBLE_EQ(fromStream.value().origin.latitude, 45.52724575);

    auto missing = map::ResolveMapOrigin({plain}, config);
    ASSERT_FALSE(missing);
}

TEST(MapConfig, RejectsConflictingOrigins) {
    map::MapFeedConfig config = MakeFeedConfig();
    Tracklet withOrigin = MakeTracklet("cam-01", 1, ObjectType::Car, {Timestamp(0)}, 1.0);
    withOrigin.origin_latitude = 45.52724575;
    withOrigin.origin_longitude = 5.97433625;

    auto conflict = map::ResolveMapOrigin({withOrigin}, config);
    ASSERT_FALSE(conflict);

    config.has_origin = false;
    Tracklet other = withOrigin;
    other.origin_latitude = 46.0;
    auto mixed = map::ResolveMapOrigin({withOrigin, other}, config);
    ASSERT_FALSE(mixed);
}

// ---------------------------------------------------------------------------
// Acceleration
// ---------------------------------------------------------------------------

TEST(AccelerationEstimator, RecoversConstantAcceleration) {
    map::AccelerationEstimator estimator(1.5, 5.0, 0.5);
    for (int i = 0; i <= 4; ++i) {
        const double t = 0.5 * static_cast<double>(i);
        estimator.Add(Timestamp(static_cast<std::int64_t>(t * 1000.0)), 2.0 * t);
    }
    const std::optional<double> estimate = estimator.Estimate(Timestamp(2000));
    ASSERT_TRUE(estimate.has_value());
    EXPECT_NEAR(*estimate, 2.0, 1e-9);
}

TEST(AccelerationEstimator, ReturnsNulloptWhenShortOrStale) {
    map::AccelerationEstimator estimator;
    EXPECT_FALSE(estimator.Estimate(Timestamp(0)).has_value());

    estimator.Add(Timestamp(0), 1.0);
    EXPECT_FALSE(estimator.Estimate(Timestamp(0)).has_value());

    estimator.Add(Timestamp(500), 2.0);
    EXPECT_TRUE(estimator.Estimate(Timestamp(1000)).has_value());
    EXPECT_FALSE(estimator.Estimate(Timestamp(2001)).has_value());
}

TEST(AccelerationEstimator, ClampsExtremeValues) {
    map::AccelerationEstimator estimator(2.0, 5.0, 1.0);
    estimator.Add(Timestamp(0), 0.0);
    estimator.Add(Timestamp(1000), 50.0);
    const std::optional<double> estimate = estimator.Estimate(Timestamp(1000));
    ASSERT_TRUE(estimate.has_value());
    EXPECT_LE(*estimate, 5.0);
    EXPECT_GE(*estimate, -5.0);
}

// ---------------------------------------------------------------------------
// Emission grid protocol and feed integration
// ---------------------------------------------------------------------------

std::vector<float> DecodeBase64Floats(const std::string& text) {
    static const std::string kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> lookup{};
    lookup.fill(-1);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        lookup[static_cast<unsigned char>(kAlphabet[i])] = static_cast<int>(i);
    }

    std::vector<std::uint8_t> bytes;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : text) {
        if (ch == '=') {
            break;
        }
        const int value = lookup[static_cast<unsigned char>(ch)];
        if (value < 0) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFu));
        }
    }

    std::vector<float> floats(bytes.size() / 4);
    for (std::size_t i = 0; i < floats.size(); ++i) {
        const std::uint32_t bits32 = static_cast<std::uint32_t>(bytes[4 * i]) |
                                     (static_cast<std::uint32_t>(bytes[4 * i + 1]) << 8) |
                                     (static_cast<std::uint32_t>(bytes[4 * i + 2]) << 16) |
                                     (static_cast<std::uint32_t>(bytes[4 * i + 3]) << 24);
        std::memcpy(&floats[i], &bits32, sizeof(float));
    }
    return floats;
}

std::string ExtractFieldData(const std::string& json, const std::string& field) {
    const std::string marker = "\"" + field + "\":{\"data\":\"";
    const std::size_t start = json.find(marker);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t begin = start + marker.size();
    const std::size_t end = json.find('"', begin);
    if (end == std::string::npos) {
        return {};
    }
    return json.substr(begin, end - begin);
}

TEST(MapProtocol, SerialisesEmissionGrid) {
    map::MapFrame frame;
    frame.type = map::MapFrameType::EmissionGrid;
    frame.timestamp = Timestamp(1234);

    map::MapEmissionGrid grid;
    grid.south_west = GeoPoint{32.60, -85.49};
    grid.north_east = GeoPoint{32.61, -85.48};
    grid.cols = 2;
    grid.rows = 2;
    grid.fields[0].values = {1.5f, 2.5f, 3.5f, 4.5f};
    grid.fields[0].max = 4.5f;
    grid.fields[0].mean = 3.0f;
    grid.fields[0].p95 = 4.4f;
    frame.emission = grid;

    const std::string json = map::ToJson(frame);
    EXPECT_NE(json.find("\"type\":\"emission_grid\""), std::string::npos);
    EXPECT_NE(json.find("\"unit\":\"ug/m3\""), std::string::npos);
    EXPECT_NE(json.find("\"cols\":2"), std::string::npos);
    EXPECT_NE(json.find("\"PM2.5\""), std::string::npos);

    const std::vector<float> decoded = DecodeBase64Floats(ExtractFieldData(json, "NOx"));
    ASSERT_EQ(decoded.size(), 4U);
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        EXPECT_FLOAT_EQ(decoded[i], grid.fields[0].values[i]);
    }
}

map::MapFeedConfig MakeEmissionFeedConfig() {
    map::MapFeedConfig config = MakeFeedConfig();
    config.emissions.enabled = true;
    config.emissions.wind.speed_mps = 3.0;
    config.emissions.wind.from_deg = 270.0;
    config.emissions.puff_interval_s = 1.0;
    config.emissions.max_age_s = 60.0;
    config.emissions.grid_hz = 1.0;
    config.emissions.grid.has_center = true;
    config.emissions.grid.center_latitude = kOriginLat;
    config.emissions.grid.center_longitude = kOriginLon;
    config.emissions.grid.width_m = 100.0;
    config.emissions.grid.height_m = 100.0;
    config.emissions.grid.cell_m = 5.0;
    return config;
}

TEST(MapFeed, EmitsEmissionGridWhenEnabled) {
    map::MapFeed feed(MakeEmissionFeedConfig());
    const Tracklet tracklet = MakeTracklet("cam-01", 1, ObjectType::Car,
                                           {Timestamp(0), Timestamp(500), Timestamp(1000), Timestamp(1500),
                                            Timestamp(2000)},
                                           5.0);

    std::size_t emission_frames = 0;
    std::size_t max_cells = 0;
    for (const Observation& observation : tracklet.observations) {
        feed.Observe(tracklet, observation);
        for (const map::MapFrame& frame : feed.Tick(observation.ts)) {
            if (frame.type != map::MapFrameType::EmissionGrid) {
                continue;
            }
            ++emission_frames;
            ASSERT_TRUE(frame.emission.has_value());
            EXPECT_EQ(frame.emission->cols, 20);
            EXPECT_EQ(frame.emission->rows, 20);
            max_cells = std::max(max_cells, frame.emission->fields[0].values.size());
        }
    }

    EXPECT_GE(emission_frames, 2U);
    EXPECT_EQ(max_cells, 400U);

    const std::optional<map::MapFrame> snapshot = feed.EmissionSnapshot(Timestamp(2000));
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->emission.has_value());
    EXPECT_GT(snapshot->emission->fields[0].max, 0.f);
    EXPECT_GT(snapshot->emission->north_east.latitude, snapshot->emission->south_west.latitude);
}

TEST(MapFeed, EmissionSnapshotIsNullWhenDisabled) {
    map::MapFeed feed(MakeFeedConfig());
    EXPECT_FALSE(feed.EmissionSnapshot(Timestamp(0)).has_value());
}

// ---------------------------------------------------------------------------
// Emission configuration
// ---------------------------------------------------------------------------

TEST(MapConfig, LoadsEmissionSection) {
    const TempFile config("tmcmt_map_emissions.yaml",
                          "feed:\n"
                          "  origin: 32.606456,-85.481891\n"
                          "emissions:\n"
                          "  enabled: true\n"
                          "  stability_class: C\n"
                          "  wind: {speed_mps: 2.5, from_deg: 180}\n"
                          "  pm25_fraction: 0.4\n"
                          "  grid_hz: 2\n"
                          "  grid:\n"
                          "    center_lat: 32.606456\n"
                          "    center_lon: -85.481891\n"
                          "    width_m: 300\n"
                          "    height_m: 200\n"
                          "    cell_m: 4\n"
                          "cameras:\n"
                          "  - id: cam-01\n"
                          "    camera_id: 1\n");

    auto loaded = map::LoadMapFeedConfig(config.Path());
    ASSERT_TRUE(loaded) << loaded.status().ToString();
    const map::MapFeedConfig& value = loaded.value();
    EXPECT_TRUE(value.emissions.enabled);
    EXPECT_EQ(value.emissions.stability, tmcmt::emissions::StabilityClass::C);
    EXPECT_DOUBLE_EQ(value.emissions.wind.speed_mps, 2.5);
    EXPECT_DOUBLE_EQ(value.emissions.wind.from_deg, 180.0);
    EXPECT_DOUBLE_EQ(value.emissions.pm25_fraction, 0.4);
    EXPECT_DOUBLE_EQ(value.emissions.grid_hz, 2.0);
    EXPECT_TRUE(value.emissions.grid.has_center);
    EXPECT_DOUBLE_EQ(value.emissions.grid.width_m, 300.0);
    EXPECT_DOUBLE_EQ(value.emissions.grid.cell_m, 4.0);
}

TEST(MapConfig, RejectsInvalidEmissionValues) {
    const TempFile config("tmcmt_map_bad_emissions.yaml",
                          "feed:\n"
                          "  origin: 32.606456,-85.481891\n"
                          "emissions:\n"
                          "  enabled: true\n"
                          "  pm25_fraction: 1.5\n"
                          "cameras:\n"
                          "  - id: cam-01\n"
                          "    camera_id: 1\n");

    auto loaded = map::LoadMapFeedConfig(config.Path());
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

TEST(MapConfig, RejectsUnknownStabilityClass) {
    const TempFile config("tmcmt_map_bad_stability.yaml",
                          "feed:\n"
                          "  origin: 32.606456,-85.481891\n"
                          "emissions:\n"
                          "  enabled: true\n"
                          "  stability_class: Q\n"
                          "cameras:\n"
                          "  - id: cam-01\n"
                          "    camera_id: 1\n");

    auto loaded = map::LoadMapFeedConfig(config.Path());
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

} // namespace
