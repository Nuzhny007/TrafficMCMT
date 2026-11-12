#pragma once

#include <array>
#include <cstdint>

#include "trafficmcmt/core/Camera.h"
#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt::bev {

struct ImageSize {
    int width = 0;
    int height = 0;
};

/// Immutable image -> ground projector built from a camera calibration.
///
/// The homography is applied in double precision and never throws. A point is
/// rejected when the projective denominator is near zero (the point is on or
/// beyond the horizon) or when the result is not finite.
class HomographyProjector {
public:
    [[nodiscard]] static Result<HomographyProjector> Create(const CameraCalibration& calibration);

    /// Projects a normalised image point (`x / width`, `y / height`).
    [[nodiscard]] WorldPoint ProjectNormalized(double u, double v) const;

    /// Projects a raw image pixel. `frame` may be empty for normalised
    /// calibrations; pixel-space calibrations fall back to the calibration's
    /// own image size when `frame` is not set.
    [[nodiscard]] WorldPoint ProjectPixel(double x, double y, ImageSize frame = {}) const;

    /// Projects the bottom-centre of a bounding box (the contact point with
    /// the ground), which is the convention used by the Edge exporter.
    [[nodiscard]] WorldPoint ProjectBBox(const BBox& bbox, ImageSize frame = {}) const;

    /// Inverse mapping in normalised image coordinates.
    [[nodiscard]] Result<std::array<double, 2>> UnprojectNormalized(const EnuPoint& ground) const;

    /// Inverse mapping in raw pixels.
    [[nodiscard]] Result<std::array<double, 2>> UnprojectPixel(const EnuPoint& ground,
                                                               ImageSize frame = {}) const;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_valid;
    }
    [[nodiscard]] bool IsNormalized() const noexcept {
        return m_normalized;
    }
    [[nodiscard]] const CameraCalibration& Calibration() const noexcept {
        return m_calibration;
    }
    [[nodiscard]] const Homography& Matrix() const noexcept {
        return m_h;
    }
    [[nodiscard]] const Homography& Inverse() const noexcept {
        return m_inverse;
    }

private:
    [[nodiscard]] ImageSize EffectiveFrame(ImageSize frame) const noexcept;
    [[nodiscard]] bool Apply(const Homography& matrix, double u, double v, double& outX,
                             double& outY) const noexcept;

    CameraCalibration m_calibration;
    Homography m_h{};
    Homography m_inverse{};
    bool m_valid = false;
    bool m_normalized = true;
};

} // namespace tmcmt::bev
