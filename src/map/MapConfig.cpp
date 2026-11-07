#include "trafficmcmt/map/MapConfig.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace tmcmt::map {
namespace {

using tmcmt::Status;

std::string Trim(std::string text) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Status Fail(const std::string& field, const std::string& message) {
    return Status::Error(ErrorCode::InvalidArgument, "map config: " + field + " " + message);
}

std::string ResolvePath(const std::string& path, const std::string& configPath) {
    if (path.empty()) {
        return path;
    }
    try {
        if (std::filesystem::exists(path)) {
            return path;
        }
        const std::filesystem::path candidate = std::filesystem::path(configPath).parent_path() / path;
        if (std::filesystem::exists(candidate)) {
            return candidate.string();
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Fall through and let the caller report a descriptive open error.
    }
    return path;
}

std::map<std::string, std::string> ParseIniSection(const std::string& path, const std::string& wantedSection,
                                                   std::string& error) {
    std::ifstream stream(path);
    if (!stream.is_open()) {
        error = "cannot open geo binding file: " + path;
        return {};
    }

    const std::string wanted = Lower(wantedSection);
    std::map<std::string, std::string> values;
    std::string currentSection;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        line = Trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            currentSection = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        if (currentSection != wanted) {
            continue;
        }
        const std::string key = Lower(Trim(line.substr(0, separator)));
        const std::string value = Trim(line.substr(separator + 1));
        if (key.empty()) {
            error = "empty key in " + path + " at line " + std::to_string(lineNumber);
            return {};
        }
        values[key] = value;
    }
    return values;
}

bool ParseDouble(const std::map<std::string, std::string>& values, const std::string& key, double& out) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return false;
    }
    std::istringstream parser(it->second);
    parser >> out;
    return parser && std::isfinite(out);
}

struct HullPoint {
    double x = 0.0;
    double y = 0.0;
    GeoPoint geo{};
};

double Cross(const HullPoint& origin, const HullPoint& a, const HullPoint& b) {
    return (a.x - origin.x) * (b.y - origin.y) - (a.y - origin.y) * (b.x - origin.x);
}

std::vector<GeoPoint> ConvexHull(std::vector<GeoPoint> points) {
    if (points.size() < 3) {
        return {};
    }

    double meanLat = 0.0;
    for (const GeoPoint& point : points) {
        meanLat += point.latitude;
    }
    meanLat /= static_cast<double>(points.size());
    const double scale = std::cos(meanLat * 0.017453292519943295769236907684886);

    std::vector<HullPoint> projected;
    projected.reserve(points.size());
    for (const GeoPoint& point : points) {
        projected.push_back(HullPoint{point.longitude * scale, point.latitude, point});
    }

    std::sort(projected.begin(), projected.end(), [](const HullPoint& a, const HullPoint& b) {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        return a.y < b.y;
    });
    projected.erase(std::unique(projected.begin(), projected.end(),
                                [](const HullPoint& a, const HullPoint& b) {
                                    return a.x == b.x && a.y == b.y;
                                }),
                    projected.end());
    if (projected.size() < 3) {
        return {};
    }

    std::vector<HullPoint> hull;
    for (const HullPoint& point : projected) {
        while (hull.size() >= 2 && Cross(hull[hull.size() - 2], hull[hull.size() - 1], point) <= 0.0) {
            hull.pop_back();
        }
        hull.push_back(point);
    }
    const std::size_t lowerSize = hull.size();
    for (std::size_t i = projected.size() - 1; i > 0; --i) {
        const HullPoint& point = projected[i - 1];
        while (hull.size() > lowerSize && Cross(hull[hull.size() - 2], hull[hull.size() - 1], point) <= 0.0) {
            hull.pop_back();
        }
        hull.push_back(point);
    }
    hull.pop_back();

    std::vector<GeoPoint> result;
    result.reserve(hull.size());
    for (const HullPoint& point : hull) {
        result.push_back(point.geo);
    }
    return result;
}

template <typename T>
bool ReadScalar(const YAML::Node& node, const char* key, T& out) {
    const YAML::Node value = node[key];
    if (!value || !value.IsScalar()) {
        return false;
    }
    out = value.as<T>();
    return true;
}

bool ReadOrigin(const YAML::Node& node, MapFeedConfig& config) {
    if (!node) {
        return false;
    }
    if (node.IsMap()) {
        if (!ReadScalar(node, "lat", config.origin_latitude) || !ReadScalar(node, "lon", config.origin_longitude)) {
            return false;
        }
        config.has_origin = true;
        return true;
    }
    if (!node.IsScalar()) {
        return false;
    }
    const std::string text = Trim(node.as<std::string>());
    const std::size_t separator = text.find(',');
    if (separator == std::string::npos) {
        return false;
    }
    std::istringstream parser(text);
    char comma = 0;
    parser >> config.origin_latitude >> comma >> config.origin_longitude;
    config.has_origin = parser && comma == ',' && std::isfinite(config.origin_latitude) &&
                        std::isfinite(config.origin_longitude);
    return config.has_origin;
}

bool ReadColor(const YAML::Node& node, std::array<int, 4>& color) {
    if (!node || !node.IsSequence() || node.size() != 4) {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        color[i] = node[i].as<int>();
        if (color[i] < 0 || color[i] > 255) {
            return false;
        }
    }
    return true;
}

bool ReadPolygon(const YAML::Node& node, std::vector<GeoPoint>& polygon) {
    if (!node || !node.IsSequence()) {
        return false;
    }
    polygon.clear();
    for (const YAML::Node& point : node) {
        if (!point.IsSequence() || point.size() != 2) {
            return false;
        }
        GeoPoint geo;
        geo.longitude = point[0].as<double>();
        geo.latitude = point[1].as<double>();
        if (!geo.IsFinite()) {
            return false;
        }
        polygon.push_back(geo);
    }
    return polygon.size() >= 3;
}

bool ReadWind(const YAML::Node& node, emissions::WindConfig& wind) {
    if (!node || !node.IsMap()) {
        return false;
    }
    return ReadScalar(node, "speed_mps", wind.speed_mps) && ReadScalar(node, "from_deg", wind.from_deg);
}

} // namespace

Result<MapFeedConfig> LoadMapFeedConfig(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& error) {
        return Status::Error(ErrorCode::ParseError, "cannot parse map config " + path + ": " + error.what());
    }
    if (!root.IsMap()) {
        return Status::Error(ErrorCode::ParseError, "map config " + path + " must be a YAML mapping");
    }

    MapFeedConfig config;
    try {
        const YAML::Node feed = root["feed"];
        if (feed && feed.IsMap()) {
            if (feed["origin"]) {
                if (!ReadOrigin(feed["origin"], config)) {
                    return Fail("feed.origin", "must be a {lat, lon} mapping or a \"lat,lon\" string");
                }
            }
            if (feed["origin_lat"] && feed["origin_lon"]) {
                config.origin_latitude = feed["origin_lat"].as<double>();
                config.origin_longitude = feed["origin_lon"].as<double>();
                config.has_origin = true;
            }
            ReadScalar(feed, "tick_hz", config.tick_hz);
            ReadScalar(feed, "trail_points", config.trail_points);
            ReadScalar(feed, "trail_min_distance_m", config.trail_min_distance_m);
            ReadScalar(feed, "velocity_window_s", config.velocity_window_s);
            ReadScalar(feed, "velocity_max_mps", config.velocity_max_mps);
            ReadScalar(feed, "track_ttl_s", config.track_ttl_s);
            ReadScalar(feed, "snapshot_interval_s", config.snapshot_interval_s);
            ReadScalar(feed, "heartbeat_s", config.heartbeat_s);
            ReadScalar(feed, "acceleration_window_s", config.acceleration_window_s);
            ReadScalar(feed, "acceleration_max_mps2", config.acceleration_max_mps2);
        }

        const YAML::Node emissionsNode = root["emissions"];
        if (emissionsNode && emissionsNode.IsMap()) {
            emissions::EmissionConfig& emission = config.emissions;
            ReadScalar(emissionsNode, "enabled", emission.enabled);
            if (emissionsNode["stability_class"]) {
                const auto stability =
                    emissions::ParseStabilityClass(emissionsNode["stability_class"].as<std::string>());
                if (!stability) {
                    return Fail("emissions.stability_class", "must be one of A, B, C, D, E, F");
                }
                emission.stability = *stability;
            }
            if (emissionsNode["wind"] && !ReadWind(emissionsNode["wind"], emission.wind)) {
                return Fail("emissions.wind", "must be a {speed_mps, from_deg} mapping");
            }
            ReadScalar(emissionsNode, "source_height_m", emission.source_height_m);
            ReadScalar(emissionsNode, "breathing_height_m", emission.breathing_height_m);
            ReadScalar(emissionsNode, "mixing_height_m", emission.mixing_height_m);
            ReadScalar(emissionsNode, "puff_interval_s", emission.puff_interval_s);
            ReadScalar(emissionsNode, "max_age_s", emission.max_age_s);
            ReadScalar(emissionsNode, "max_puffs", emission.max_puffs);
            ReadScalar(emissionsNode, "pm25_fraction", emission.pm25_fraction);
            ReadScalar(emissionsNode, "grid_hz", emission.grid_hz);
            ReadScalar(emissionsNode, "min_peak_ug_m3", emission.min_peak_ug_m3);
            ReadScalar(emissionsNode, "clamp_negative_acceleration", emission.clamp_negative_acceleration);

            if (emissionsNode["grid"]) {
                const YAML::Node grid = emissionsNode["grid"];
                if (!grid.IsMap()) {
                    return Fail("emissions.grid", "must be a mapping");
                }
                const bool hasLat = static_cast<bool>(grid["center_lat"]);
                const bool hasLon = static_cast<bool>(grid["center_lon"]);
                if (hasLat != hasLon) {
                    return Fail("emissions.grid.center_*", "center_lat and center_lon must be set together");
                }
                if (hasLat) {
                    emission.grid.center_latitude = grid["center_lat"].as<double>();
                    emission.grid.center_longitude = grid["center_lon"].as<double>();
                    emission.grid.has_center = true;
                }
                ReadScalar(grid, "width_m", emission.grid.width_m);
                ReadScalar(grid, "height_m", emission.grid.height_m);
                ReadScalar(grid, "cell_m", emission.grid.cell_m);
                ReadScalar(grid, "max_cols", emission.grid.max_cols);
                ReadScalar(grid, "max_rows", emission.grid.max_rows);
            }
        }

        const YAML::Node cameras = root["cameras"];
        if (!cameras || !cameras.IsSequence() || cameras.size() == 0) {
            return Fail("cameras", "must be a non-empty sequence");
        }

        std::set<int> cameraNumbers;
        std::set<std::string> cameraIds;
        for (const YAML::Node& node : cameras) {
            MapCameraConfig camera;
            if (!ReadScalar(node, "id", camera.id) || camera.id.empty()) {
                return Fail("cameras[].id", "must be a non-empty string");
            }
            if (!ReadScalar(node, "camera_id", camera.camera_id) || camera.camera_id < 0) {
                return Fail("cameras[].camera_id", "must be a non-negative integer");
            }
            if (!cameraNumbers.insert(camera.camera_id).second) {
                return Fail("cameras[].camera_id", "contains a duplicate value " + std::to_string(camera.camera_id));
            }
            if (!cameraIds.insert(camera.id).second) {
                return Fail("cameras[].id", "contains a duplicate value '" + camera.id + "'");
            }

            if (node["color"] && !ReadColor(node["color"], camera.color)) {
                return Fail("cameras[].color", "must be [r, g, b, a] with values in 0..255");
            }
            if (node["polygon"] && !ReadPolygon(node["polygon"], camera.polygon)) {
                return Fail("cameras[].polygon", "must contain at least three [lon, lat] points");
            }
            if (node["geo_bind"]) {
                camera.geo_bind = ResolvePath(node["geo_bind"].as<std::string>(), path);
            }
            if (camera.polygon.empty() && !camera.geo_bind.empty()) {
                auto polygon = LoadGeoBindingPolygon(camera.geo_bind);
                if (!polygon) {
                    return polygon.status();
                }
                camera.polygon = std::move(polygon).value();
            }
            config.cameras.push_back(std::move(camera));
        }
    } catch (const YAML::Exception& error) {
        return Status::Error(ErrorCode::ParseError, "invalid map config " + path + ": " + error.what());
    }

    if (config.has_origin &&
        (!std::isfinite(config.origin_latitude) || !std::isfinite(config.origin_longitude))) {
        return Fail("feed.origin", "must be finite");
    }
    if (config.tick_hz < 1 || config.tick_hz > 60) {
        return Fail("feed.tick_hz", "must be in 1..60");
    }
    if (config.trail_points < 2 || config.trail_points > 64) {
        return Fail("feed.trail_points", "must be in 2..64");
    }
    if (config.trail_min_distance_m < 0.0) {
        return Fail("feed.trail_min_distance_m", "must be >= 0");
    }
    if (!(config.velocity_window_s > 0.0) || !(config.velocity_max_mps > 0.0)) {
        return Fail("feed.velocity_*", "must be positive");
    }
    if (!(config.track_ttl_s > 0.0)) {
        return Fail("feed.track_ttl_s", "must be > 0");
    }
    if (config.snapshot_interval_s < 0.0) {
        return Fail("feed.snapshot_interval_s", "must be >= 0");
    }
    if (!(config.heartbeat_s > 0.0)) {
        return Fail("feed.heartbeat_s", "must be > 0");
    }
    if (!(config.acceleration_window_s > 0.0) || !(config.acceleration_max_mps2 > 0.0)) {
        return Fail("feed.acceleration_*", "must be positive");
    }

    const emissions::EmissionConfig& emission = config.emissions;
    if (!std::isfinite(emission.wind.speed_mps) || emission.wind.speed_mps < 0.0) {
        return Fail("emissions.wind.speed_mps", "must be >= 0");
    }
    if (!std::isfinite(emission.wind.from_deg)) {
        return Fail("emissions.wind.from_deg", "must be finite");
    }
    if (!(emission.source_height_m > 0.0) || !(emission.breathing_height_m > 0.0)) {
        return Fail("emissions.*_height_m", "must be > 0");
    }
    if (!(emission.mixing_height_m > 0.0)) {
        return Fail("emissions.mixing_height_m", "must be > 0");
    }
    if (!(emission.puff_interval_s > 0.0)) {
        return Fail("emissions.puff_interval_s", "must be > 0");
    }
    if (!(emission.max_age_s > 0.0)) {
        return Fail("emissions.max_age_s", "must be > 0");
    }
    if (emission.max_puffs == 0) {
        return Fail("emissions.max_puffs", "must be >= 1");
    }
    if (emission.pm25_fraction < 0.0 || emission.pm25_fraction > 1.0) {
        return Fail("emissions.pm25_fraction", "must be in 0..1");
    }
    if (!(emission.grid_hz > 0.0)) {
        return Fail("emissions.grid_hz", "must be > 0");
    }
    if (emission.min_peak_ug_m3 < 0.0) {
        return Fail("emissions.min_peak_ug_m3", "must be >= 0");
    }
    if (!(emission.grid.cell_m > 0.0) || !(emission.grid.width_m > 0.0) ||
        !(emission.grid.height_m > 0.0)) {
        return Fail("emissions.grid", "cell_m, width_m and height_m must be > 0");
    }
    if (emission.grid.max_cols < 1 || emission.grid.max_rows < 1) {
        return Fail("emissions.grid.max_*", "must be >= 1");
    }

    return config;
}

Result<std::vector<GeoPoint>> LoadGeoBindingPolygon(const std::string& path) {
    std::string error;
    const auto values = ParseIniSection(path, "points", error);
    if (!error.empty()) {
        return Status::Error(ErrorCode::IoError, error);
    }
    if (values.empty()) {
        return Status::Error(ErrorCode::ParseError, "geo binding file has no [points] section: " + path);
    }

    std::vector<GeoPoint> points;
    for (std::size_t index = 0;; ++index) {
        const std::string suffix = std::to_string(index);
        const std::string latKey = "lat" + suffix;
        const std::string lonKey = "lon" + suffix;
        const bool hasLat = values.find(latKey) != values.end();
        const bool hasLon = values.find(lonKey) != values.end();
        if (!hasLat && !hasLon) {
            break;
        }
        if (!hasLat || !hasLon) {
            return Status::Error(ErrorCode::ParseError,
                                 "incomplete geo control point " + suffix + " in " + path);
        }
        GeoPoint point;
        if (!ParseDouble(values, latKey, point.latitude) || !ParseDouble(values, lonKey, point.longitude)) {
            return Status::Error(ErrorCode::ParseError,
                                 "invalid numeric value in geo control point " + suffix + " in " + path);
        }
        points.push_back(point);
    }

    if (points.size() < 3) {
        return Status::Error(ErrorCode::ParseError,
                             "geo binding needs at least 3 control points for a zone, got " +
                                 std::to_string(points.size()));
    }

    std::vector<GeoPoint> hull = ConvexHull(std::move(points));
    if (hull.size() < 3) {
        return Status::Error(ErrorCode::ParseError,
                             "geo binding control points are degenerate and do not form a zone: " + path);
    }
    return hull;
}

Result<MapOriginResolution> ResolveMapOrigin(const std::vector<Tracklet>& tracklets, const MapFeedConfig& config) {
    bool hasStreamOrigin = false;
    GeoPoint streamOrigin{};
    for (const Tracklet& tracklet : tracklets) {
        if (!tracklet.HasOrigin()) {
            continue;
        }
        if (!hasStreamOrigin) {
            hasStreamOrigin = true;
            streamOrigin = GeoPoint{tracklet.origin_latitude, tracklet.origin_longitude};
            continue;
        }
        if (std::fabs(streamOrigin.latitude - tracklet.origin_latitude) > 1e-9 ||
            std::fabs(streamOrigin.longitude - tracklet.origin_longitude) > 1e-9) {
            return Status::Error(
                ErrorCode::InvalidArgument,
                "tracklet stream mixes ENU origins (" + std::to_string(streamOrigin.latitude) + "," +
                    std::to_string(streamOrigin.longitude) + ") and (" +
                    std::to_string(tracklet.origin_latitude) + "," +
                    std::to_string(tracklet.origin_longitude) + "); re-export with a shared --geo-origin");
        }
    }

    MapOriginResolution resolution;
    if (config.has_origin) {
        if (!std::isfinite(config.origin_latitude) || !std::isfinite(config.origin_longitude)) {
            return Status::Error(ErrorCode::InvalidArgument, "configured map origin is not finite");
        }
        if (hasStreamOrigin &&
            (std::fabs(streamOrigin.latitude - config.origin_latitude) > 1e-9 ||
             std::fabs(streamOrigin.longitude - config.origin_longitude) > 1e-9)) {
            return Status::Error(
                ErrorCode::InvalidArgument,
                "configured map origin (" + std::to_string(config.origin_latitude) + "," +
                    std::to_string(config.origin_longitude) + ") does not match the tracklet stream (" +
                    std::to_string(streamOrigin.latitude) + "," + std::to_string(streamOrigin.longitude) + ")");
        }
        resolution.origin = GeoPoint{config.origin_latitude, config.origin_longitude};
        resolution.from_stream = false;
        return resolution;
    }

    if (!hasStreamOrigin) {
        return Status::Error(
            ErrorCode::InvalidArgument,
            "no ENU origin: set feed.origin in the map config or re-export tracklets with origin fields");
    }

    resolution.origin = streamOrigin;
    resolution.from_stream = true;
    return resolution;
}

const MapCameraConfig* MapFeedConfig::FindCamera(const CameraId& id) const noexcept {    for (const MapCameraConfig& camera : cameras) {
        if (camera.id == id) {
            return &camera;
        }
    }
    return nullptr;
}

int MapFeedConfig::CameraNumber(const CameraId& id) const noexcept {
    const MapCameraConfig* camera = FindCamera(id);
    return camera != nullptr ? camera->camera_id : -1;
}

} // namespace tmcmt::map
