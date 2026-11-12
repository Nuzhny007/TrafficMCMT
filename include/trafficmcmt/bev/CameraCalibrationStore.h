#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "trafficmcmt/core/Camera.h"
#include "trafficmcmt/core/Result.h"

namespace tmcmt::bev {

/// Validation policy applied to every calibration entering the store.
struct CalibrationStoreOptions {
    bool require_homography = false;
    /// Reject a calibration whose reported RMS exceeds this value.
    /// Zero disables the check.
    double max_reprojection_error_m = 2.0;
};

/// Thread-safe camera calibration registry with optional hot reload.
///
/// Readers obtain an immutable `shared_ptr<const CameraCalibration>` snapshot,
/// so the projection hot path never holds the store lock. `ReloadIfChanged()`
/// parses the YAML file outside the lock and atomically swaps the map only when
/// every calibration in the new file is valid.
class CameraCalibrationStore {
public:
    CameraCalibrationStore() = default;
    CameraCalibrationStore(CameraCalibrationStore&&) noexcept = default;
    CameraCalibrationStore& operator=(CameraCalibrationStore&&) noexcept = default;
    CameraCalibrationStore(const CameraCalibrationStore&) = delete;
    CameraCalibrationStore& operator=(const CameraCalibrationStore&) = delete;

    /// Loads `calibrations:` (sequence) or `cameras:` (mapping) from YAML.
    [[nodiscard]] static Result<CameraCalibrationStore> LoadYaml(
        const std::string& path, CalibrationStoreOptions options = {});

    /// Builds a store from already decoded calibrations (e.g. protobuf).
    [[nodiscard]] static Result<CameraCalibrationStore> FromCalibrations(
        std::vector<CameraCalibration> calibrations, CalibrationStoreOptions options = {});

    [[nodiscard]] Status Upsert(CameraCalibration calibration);
    [[nodiscard]] bool Remove(const CameraId& camera);

    /// Returns null when the camera has no calibration.
    [[nodiscard]] std::shared_ptr<const CameraCalibration> Get(const CameraId& camera) const;

    [[nodiscard]] std::vector<CameraId> Cameras() const;
    [[nodiscard]] std::vector<std::shared_ptr<const CameraCalibration>> Snapshot() const;

    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] std::uint64_t ReloadCount() const noexcept;

    [[nodiscard]] const std::string& Path() const noexcept {
        return m_path;
    }
    [[nodiscard]] const CalibrationStoreOptions& Options() const noexcept {
        return m_options;
    }

    /// Re-reads the YAML file when its modification time changed. On any parse
    /// or validation error the previous snapshot is kept intact.
    [[nodiscard]] Status ReloadIfChanged();

    [[nodiscard]] Status Validate(const CameraCalibration& calibration) const;

private:
    [[nodiscard]] Status Apply(std::vector<CameraCalibration> calibrations, bool fromReload);
    void BumpGeneration() noexcept;

    CalibrationStoreOptions m_options;
    std::string m_path;

    mutable std::unique_ptr<std::shared_mutex> m_mutex = std::make_unique<std::shared_mutex>();
    std::map<CameraId, std::shared_ptr<const CameraCalibration>> m_calibrations;
    std::uint64_t m_generation = 0;
    std::uint64_t m_reloadCount = 0;

    std::filesystem::file_time_type m_mtime{};
    bool m_hasMtime = false;
};

} // namespace tmcmt::bev
