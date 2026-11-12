#include "trafficmcmt/bev/CameraCalibrationStore.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace tmcmt::bev {
namespace {

Status ParseError(const std::string& where, const std::string& what) {
    return Status::Error(ErrorCode::ParseError, where + ": " + what);
}

Status Invalid(const std::string& where, const std::string& what) {
    return Status::Error(ErrorCode::InvalidArgument, where + ": " + what);
}

std::string Join(const std::string& prefix, const char* key) {
    return prefix.empty() ? std::string(key) : prefix + "." + key;
}

template <typename T>
Status ReadScalar(const YAML::Node& node, const char* key, T& out, const std::string& where,
                  bool required = false) {
    const YAML::Node value = node[key];
    if (!value || value.IsNull()) {
        if (required) {
            return ParseError(Join(where, key), "is required");
        }
        return Status::Ok();
    }
    try {
        out = value.as<T>();
    } catch (const YAML::Exception& e) {
        return ParseError(Join(where, key), e.what());
    }
    return Status::Ok();
}

Status ReadStringAlias(const YAML::Node& node, const char* primary, const char* alias, std::string& out,
                       const std::string& where) {
    Status status = ReadScalar(node, primary, out, where);
    if (!status.ok()) {
        return status;
    }
    if (!out.empty() || !alias) {
        return Status::Ok();
    }
    return ReadScalar(node, alias, out, where);
}

Status ReadDoubleAlias(const YAML::Node& node, const char* primary, const char* alias, double& out,
                       const std::string& where) {
    Status status = ReadScalar(node, primary, out, where);
    if (!status.ok()) {
        return status;
    }
    if (!alias || node[primary]) {
        return Status::Ok();
    }
    return ReadScalar(node, alias, out, where);
}

Status ParseHomography(const YAML::Node& node, Homography& out, bool& has, const std::string& where) {
    const YAML::Node value = node["homography"];
    if (!value || value.IsNull()) {
        Status status = ReadScalar(node, "has_homography", has, where);
        return status;
    }
    if (!value.IsSequence()) {
        return ParseError(Join(where, "homography"), "expected a list of 9 numbers");
    }

    std::vector<double> flat;
    flat.reserve(9);
    if (value.size() == 3 && value[0].IsSequence()) {
        for (const YAML::Node& row : value) {
            if (!row.IsSequence() || row.size() != 3) {
                return ParseError(Join(where, "homography"), "expected a 3x3 matrix");
            }
            for (const YAML::Node& item : row) {
                flat.push_back(item.as<double>());
            }
        }
    } else {
        for (const YAML::Node& item : value) {
            flat.push_back(item.as<double>());
        }
    }
    if (flat.size() != out.size()) {
        return ParseError(Join(where, "homography"), "expected exactly 9 values");
    }
    std::copy(flat.begin(), flat.end(), out.begin());
    has = true;
    return Status::Ok();
}

Status ParsePolygon(const YAML::Node& node, const char* key, Polygon& out, const std::string& where) {
    const YAML::Node value = node[key];
    if (!value || value.IsNull()) {
        return Status::Ok();
    }
    const YAML::Node xy = value.IsMap() ? value["xy"] : value;
    if (!xy || !xy.IsSequence()) {
        return ParseError(Join(where, key), "expected a flat list of coordinates");
    }
    out.xy.clear();
    out.xy.reserve(xy.size());
    for (const YAML::Node& item : xy) {
        out.xy.push_back(item.as<float>());
    }
    if (!out.xy.empty() && !out.IsValid()) {
        return ParseError(Join(where, key), "expected at least 3 points (6 coordinates)");
    }
    return Status::Ok();
}

Result<CameraCalibration> ParseOne(const YAML::Node& node, const std::string& fallbackCamera,
                                   const std::string& where) {
    if (!node.IsMap()) {
        return ParseError(where, "expected a mapping");
    }

    CameraCalibration out;
    out.camera_id = fallbackCamera;
    Status status = ReadStringAlias(node, "camera_id", nullptr, out.camera_id, where);
    if (!status.ok()) {
        return status;
    }
    if (out.camera_id.empty()) {
        return ParseError(where, "camera_id is required");
    }

    if (!(status = ReadScalar(node, "has_homography", out.has_homography, where)).ok()) {
        return status;
    }
    if (!(status = ParseHomography(node, out.homography, out.has_homography, where)).ok()) {
        return status;
    }
    if (!(status = ParsePolygon(node, "entry_zone", out.entry_zone, where)).ok()) {
        return status;
    }
    if (!(status = ParsePolygon(node, "exit_zone", out.exit_zone, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "meters_per_pixel_at_ground", out.meters_per_pixel_at_ground, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "frame_id", out.frame_id, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "updated_at_ms", out.updated_at_ms, where)).ok()) {
        return status;
    }
    if (!(status = ReadDoubleAlias(node, "origin_lat", "origin_latitude", out.origin_latitude, where)).ok()) {
        return status;
    }
    if (!(status = ReadDoubleAlias(node, "origin_lon", "origin_longitude", out.origin_longitude, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "image_width", out.image_width, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "image_height", out.image_height, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "homography_normalized", out.homography_normalized, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "reprojection_error_m", out.reprojection_error_m, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "source", out.source, where)).ok()) {
        return status;
    }
    if (!(status = ReadScalar(node, "version", out.version, where)).ok()) {
        return status;
    }
    if (out.source.empty()) {
        out.source = "manual";
    }
    if (out.version == 0 && out.updated_at_ms > 0) {
        out.version = static_cast<std::uint64_t>(out.updated_at_ms);
    }

    return out;
}

Result<std::vector<CameraCalibration>> ParseYamlFile(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::BadFile&) {
        return Status::Error(ErrorCode::IoError, "cannot open calibration file: " + path);
    } catch (const YAML::Exception& e) {
        return Status::Error(ErrorCode::ParseError, path + ": " + e.what());
    } catch (const std::exception& e) {
        return Status::Error(ErrorCode::Internal, path + ": " + e.what());
    }

    std::vector<CameraCalibration> out;
    const auto append = [&out](Result<CameraCalibration> parsed) -> Status {
        if (!parsed) {
            return parsed.status();
        }
        out.push_back(std::move(parsed).value());
        return Status::Ok();
    };

    try {
        if (root.IsSequence()) {
            for (std::size_t i = 0; i < root.size(); ++i) {
                const std::string where = path + "[" + std::to_string(i) + "]";
                Status status = append(ParseOne(root[i], {}, where));
                if (!status.ok()) {
                    return status;
                }
            }
            return out;
        }
        if (!root.IsMap()) {
            return Status::Error(ErrorCode::ParseError, path + ": root element must be a mapping or a list");
        }

        const YAML::Node list = root["calibrations"];
        if (list && !list.IsNull()) {
            if (!list.IsSequence()) {
                return ParseError(path, "calibrations must be a list");
            }
            for (std::size_t i = 0; i < list.size(); ++i) {
                const std::string where = path + ".calibrations[" + std::to_string(i) + "]";
                Status status = append(ParseOne(list[i], {}, where));
                if (!status.ok()) {
                    return status;
                }
            }
            return out;
        }

        const YAML::Node cameras = root["cameras"];
        if (cameras && !cameras.IsNull()) {
            if (!cameras.IsMap()) {
                return ParseError(path, "cameras must be a mapping");
            }
            for (const auto& entry : cameras) {
                const std::string camera = entry.first.as<std::string>();
                Status status = append(ParseOne(entry.second, camera, path + ".cameras." + camera));
                if (!status.ok()) {
                    return status;
                }
            }
            return out;
        }

        // Bare mapping: camera id -> calibration.
        for (const auto& entry : root) {
            if (!entry.second.IsMap()) {
                continue;
            }
            const std::string camera = entry.first.as<std::string>();
            Status status = append(ParseOne(entry.second, camera, path + "." + camera));
            if (!status.ok()) {
                return status;
            }
        }
    } catch (const YAML::Exception& e) {
        return Status::Error(ErrorCode::ParseError, path + ": " + e.what());
    }

    if (out.empty()) {
        return Status::Error(ErrorCode::ParseError, path + ": no calibrations found");
    }
    return out;
}

} // namespace

Result<CameraCalibrationStore> CameraCalibrationStore::LoadYaml(const std::string& path,
                                                                CalibrationStoreOptions options) {
    auto parsed = ParseYamlFile(path);
    if (!parsed) {
        return parsed.status();
    }

    CameraCalibrationStore store;
    store.m_options = options;
    store.m_path = path;
    Status status = store.Apply(std::move(parsed).value(), false);
    if (!status.ok()) {
        return status;
    }

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        store.m_mtime = mtime;
        store.m_hasMtime = true;
    }
    return store;
}

Result<CameraCalibrationStore> CameraCalibrationStore::FromCalibrations(
    std::vector<CameraCalibration> calibrations, CalibrationStoreOptions options) {
    CameraCalibrationStore store;
    store.m_options = options;
    Status status = store.Apply(std::move(calibrations), false);
    if (!status.ok()) {
        return status;
    }
    return store;
}

Status CameraCalibrationStore::Validate(const CameraCalibration& calibration) const {
    const std::string where = "calibration '" + calibration.camera_id + "'";
    if (!calibration.IsValid()) {
        return Invalid(where, "camera id is empty, the homography is degenerate, or the image size is missing");
    }
    if (m_options.require_homography && !calibration.has_homography) {
        return Invalid(where, "a homography is required");
    }
    if (calibration.has_homography && calibration.reprojection_error_m > 0.f &&
        m_options.max_reprojection_error_m > 0.0 &&
        static_cast<double>(calibration.reprojection_error_m) > m_options.max_reprojection_error_m) {
        return Invalid(where, "reprojection error " + std::to_string(calibration.reprojection_error_m) +
                                  " m exceeds the configured maximum");
    }
    return Status::Ok();
}

Status CameraCalibrationStore::Apply(std::vector<CameraCalibration> calibrations, bool fromReload) {
    std::map<CameraId, std::shared_ptr<const CameraCalibration>> next;
    for (CameraCalibration& calibration : calibrations) {
        Status status = Validate(calibration);
        if (!status.ok()) {
            return status;
        }
        // Copy the key before moving the calibration into the snapshot: the
        // argument evaluation order of `emplace` is unspecified, and reading
        // `calibration.camera_id` after the move would yield an empty key.
        const CameraId cameraId = calibration.camera_id;
        if (next.find(cameraId) != next.end()) {
            return Invalid("calibration '" + cameraId + "'", "duplicate camera id");
        }
        const auto existing = m_calibrations.find(cameraId);
        if (existing != m_calibrations.end() && calibration.version > 0 && existing->second->version > 0 &&
            calibration.version < existing->second->version) {
            return Invalid("calibration '" + cameraId + "'",
                           "version " + std::to_string(calibration.version) + " is older than " +
                               std::to_string(existing->second->version));
        }
        next.emplace(cameraId, std::make_shared<const CameraCalibration>(std::move(calibration)));
    }

    m_calibrations = std::move(next);
    BumpGeneration();
    if (fromReload) {
        ++m_reloadCount;
    }
    return Status::Ok();
}

Status CameraCalibrationStore::Upsert(CameraCalibration calibration) {
    Status status = Validate(calibration);
    if (!status.ok()) {
        return status;
    }

    std::unique_lock lock(*m_mutex);
    const CameraId cameraId = calibration.camera_id;
    const auto existing = m_calibrations.find(cameraId);
    if (existing != m_calibrations.end() && calibration.version > 0 && existing->second->version > 0 &&
        calibration.version < existing->second->version) {
        return Invalid("calibration '" + cameraId + "'",
                       "version " + std::to_string(calibration.version) + " is older than " +
                           std::to_string(existing->second->version));
    }
    m_calibrations[cameraId] = std::make_shared<const CameraCalibration>(std::move(calibration));
    BumpGeneration();
    return Status::Ok();
}

bool CameraCalibrationStore::Remove(const CameraId& camera) {
    std::unique_lock lock(*m_mutex);
    const bool removed = m_calibrations.erase(camera) > 0;
    if (removed) {
        BumpGeneration();
    }
    return removed;
}

std::shared_ptr<const CameraCalibration> CameraCalibrationStore::Get(const CameraId& camera) const {
    std::shared_lock lock(*m_mutex);
    const auto it = m_calibrations.find(camera);
    return it == m_calibrations.end() ? nullptr : it->second;
}

std::vector<CameraId> CameraCalibrationStore::Cameras() const {
    std::shared_lock lock(*m_mutex);
    std::vector<CameraId> out;
    out.reserve(m_calibrations.size());
    for (const auto& entry : m_calibrations) {
        out.push_back(entry.first);
    }
    return out;
}

std::vector<std::shared_ptr<const CameraCalibration>> CameraCalibrationStore::Snapshot() const {
    std::shared_lock lock(*m_mutex);
    std::vector<std::shared_ptr<const CameraCalibration>> out;
    out.reserve(m_calibrations.size());
    for (const auto& entry : m_calibrations) {
        out.push_back(entry.second);
    }
    return out;
}

std::size_t CameraCalibrationStore::Size() const noexcept {
    std::shared_lock lock(*m_mutex);
    return m_calibrations.size();
}

std::uint64_t CameraCalibrationStore::Generation() const noexcept {
    std::shared_lock lock(*m_mutex);
    return m_generation;
}

std::uint64_t CameraCalibrationStore::ReloadCount() const noexcept {
    std::shared_lock lock(*m_mutex);
    return m_reloadCount;
}

Status CameraCalibrationStore::ReloadIfChanged() {
    if (m_path.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "calibration store has no file to reload");
    }

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(m_path, ec);
    if (ec) {
        return Status::Error(ErrorCode::IoError, "cannot stat calibration file: " + m_path);
    }

    std::unique_lock lock(*m_mutex);
    if (m_hasMtime && mtime == m_mtime) {
        return Status::Ok();
    }

    auto parsed = ParseYamlFile(m_path);
    if (!parsed) {
        return parsed.status();
    }
    Status status = Apply(std::move(parsed).value(), true);
    if (!status.ok()) {
        return status;
    }
    m_mtime = mtime;
    m_hasMtime = true;
    return Status::Ok();
}

void CameraCalibrationStore::BumpGeneration() noexcept {
    ++m_generation;
}

} // namespace tmcmt::bev
