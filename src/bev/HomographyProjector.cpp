#include "trafficmcmt/bev/HomographyProjector.h"

#include <algorithm>
#include <cmath>

namespace tmcmt::bev {
namespace {

constexpr double kDenominatorEpsilon = 1e-9;
constexpr double kDeterminantEpsilon = 1e-12;

bool Invert(const Homography& h, Homography& out) noexcept {
    const double a = h[0];
    const double b = h[1];
    const double c = h[2];
    const double d = h[3];
    const double e = h[4];
    const double f = h[5];
    const double g = h[6];
    const double i = h[7];
    const double j = h[8];

    const double det = a * (e * j - f * i) - b * (d * j - f * g) + c * (d * i - e * g);
    if (!std::isfinite(det) || std::fabs(det) < kDeterminantEpsilon) {
        return false;
    }
    const double inv = 1.0 / det;
    out = {(e * j - f * i) * inv, (c * i - b * j) * inv, (b * f - c * e) * inv,
           (f * g - d * j) * inv, (a * j - c * g) * inv, (c * d - a * f) * inv,
           (d * i - e * g) * inv, (b * g - a * i) * inv, (a * e - b * d) * inv};
    return true;
}

} // namespace

Result<HomographyProjector> HomographyProjector::Create(const CameraCalibration& calibration) {
    if (!calibration.IsValid()) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "calibration '" + calibration.camera_id + "' is invalid");
    }
    if (!calibration.has_homography) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "calibration '" + calibration.camera_id + "' has no homography");
    }

    HomographyProjector projector;
    projector.m_calibration = calibration;
    projector.m_h = calibration.homography;
    projector.m_normalized = calibration.homography_normalized;
    if (!Invert(projector.m_h, projector.m_inverse)) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "calibration '" + calibration.camera_id + "' homography is not invertible");
    }
    projector.m_valid = true;
    return projector;
}

ImageSize HomographyProjector::EffectiveFrame(ImageSize frame) const noexcept {
    if (frame.width > 0 && frame.height > 0) {
        return frame;
    }
    return ImageSize{m_calibration.image_width, m_calibration.image_height};
}

bool HomographyProjector::Apply(const Homography& matrix, double u, double v, double& outX,
                                double& outY) const noexcept {
    const double denominator = matrix[6] * u + matrix[7] * v + matrix[8];
    if (!std::isfinite(denominator) || std::fabs(denominator) < kDenominatorEpsilon) {
        return false;
    }
    const double x = (matrix[0] * u + matrix[1] * v + matrix[2]) / denominator;
    const double y = (matrix[3] * u + matrix[4] * v + matrix[5]) / denominator;
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    outX = x;
    outY = y;
    return true;
}

WorldPoint HomographyProjector::ProjectNormalized(double u, double v) const {
    WorldPoint out;
    if (!m_valid || !std::isfinite(u) || !std::isfinite(v)) {
        return out;
    }
    double x = 0.0;
    double y = 0.0;
    if (!Apply(m_h, u, v, x, y)) {
        return out;
    }
    out.x = static_cast<float>(x);
    out.y = static_cast<float>(y);
    out.valid = true;
    return out;
}

WorldPoint HomographyProjector::ProjectPixel(double x, double y, ImageSize frame) const {
    if (!m_valid || !std::isfinite(x) || !std::isfinite(y)) {
        return WorldPoint{};
    }
    if (!m_normalized) {
        double gx = 0.0;
        double gy = 0.0;
        if (!Apply(m_h, x, y, gx, gy)) {
            return WorldPoint{};
        }
        return WorldPoint{static_cast<float>(gx), static_cast<float>(gy), true};
    }

    const ImageSize size = EffectiveFrame(frame);
    if (size.width <= 0 || size.height <= 0) {
        return WorldPoint{};
    }
    return ProjectNormalized(x / static_cast<double>(size.width),
                             y / static_cast<double>(size.height));
}

WorldPoint HomographyProjector::ProjectBBox(const BBox& bbox, ImageSize frame) const {
    if (!bbox.IsFinite() || bbox.w < 0.f || bbox.h < 0.f) {
        return WorldPoint{};
    }
    return ProjectPixel(static_cast<double>(bbox.x) + static_cast<double>(bbox.w) * 0.5,
                        static_cast<double>(bbox.y) + static_cast<double>(bbox.h), frame);
}

Result<std::array<double, 2>> HomographyProjector::UnprojectNormalized(const EnuPoint& ground) const {
    if (!m_valid) {
        return Status::Error(ErrorCode::InvalidArgument, "projector is not valid");
    }
    if (!ground.IsFinite()) {
        return Status::Error(ErrorCode::InvalidArgument, "ground point is not finite");
    }
    double u = 0.0;
    double v = 0.0;
    if (!Apply(m_inverse, ground.east, ground.north, u, v)) {
        return Status::Error(ErrorCode::InvalidArgument, "ground point cannot be mapped to the image");
    }
    return std::array<double, 2>{u, v};
}

Result<std::array<double, 2>> HomographyProjector::UnprojectPixel(const EnuPoint& ground,
                                                                  ImageSize frame) const {
    auto normalized = UnprojectNormalized(ground);
    if (!normalized) {
        return normalized.status();
    }
    if (!m_normalized) {
        return normalized;
    }
    const ImageSize size = EffectiveFrame(frame);
    if (size.width <= 0 || size.height <= 0) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "image size is required for a normalised homography");
    }
    return std::array<double, 2>{normalized.value()[0] * static_cast<double>(size.width),
                                 normalized.value()[1] * static_cast<double>(size.height)};
}

} // namespace tmcmt::bev
