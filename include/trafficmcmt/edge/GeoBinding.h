#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt {

/// One manual correspondence between a geographic point and an image point.
///
/// `pixel` is normalised to [0, 1] and therefore independent of the frame
/// resolution, exactly as in the Multitarget-tracker `geo_bind.ini` format.
struct GeoControlPoint {
    double latitude = 0.0;
    double longitude = 0.0;
    cv::Point2d pixel{0.0, 0.0};
};

struct GeoBindingOptions {
    /// Reject a binding whose RMS reprojection error exceeds this value.
    double max_rms_error_m = 10.0;
    /// Reject a binding whose RMS reprojection error exceeds this fraction of
    /// the frame size (normalised pixels, 0.02 = 2%).
    double max_rms_error_px = 0.02;
    /// Optional shared ENU origin. When unset, the mean of the control points is
    /// used. Multi-camera setups must pass the same origin to every camera,
    /// otherwise their world coordinates are translated relative to each other.
    bool has_origin = false;
    double origin_latitude = 0.0;
    double origin_longitude = 0.0;
};

/// Converts WGS84 latitude/longitude to a local East-North-Up plane in metres.
///
/// The origin is `(lat0, lon0)`. The result is `(east, north)` in metres. This
/// mirrors the WGS84 ellipsoid formulas used by Multitarget-tracker's
/// `LocalPlane`, so both projects agree on world coordinates.
[[nodiscard]] cv::Point2d GeographicToEnu(double latitude, double longitude, double lat0, double lon0);

/// Image-to-ground homography built from `geo_bind.ini` control points.
///
/// The class is immutable after construction and safe to share between
/// threads. It has no dependency on Multitarget-tracker or GeographicLib.
class GeoBinding {
public:
    /// Parses the `[points]` section of a Multitarget-tracker `geo_bind.ini`.
    [[nodiscard]] static Result<GeoBinding> LoadFromFile(const std::string& path,
                                                         GeoBindingOptions options = {});

    /// Builds a binding from already parsed control points.
    [[nodiscard]] static Result<GeoBinding> FromControlPoints(std::vector<GeoControlPoint> points,
                                                              GeoBindingOptions options = {});

    [[nodiscard]] bool Empty() const noexcept {
        return m_points.size() < 4;
    }

    [[nodiscard]] std::size_t ControlPointCount() const noexcept {
        return m_points.size();
    }

    /// Projects a normalised image point (`x / width`, `y / height`) to metres.
    [[nodiscard]] WorldPoint ProjectNormalized(const cv::Point2d& normalized) const;

    /// Projects an image pixel to metres.
    [[nodiscard]] WorldPoint Project(const cv::Point2d& pixel, cv::Size frameSize) const;

    /// Projects an image pixel given as `float` (MTT `RotatedRect` centres).
    [[nodiscard]] WorldPoint Project(const cv::Point2f& pixel, cv::Size frameSize) const;

    /// Inverse mapping: local ENU metres back to a normalised image point.
    [[nodiscard]] Result<cv::Point2d> ToNormalizedPixel(const WorldPoint& world) const;

    [[nodiscard]] double OriginLatitude() const noexcept {
        return m_originLat;
    }

    [[nodiscard]] double OriginLongitude() const noexcept {
        return m_originLon;
    }

    [[nodiscard]] double RmsErrorMeters() const noexcept {
        return m_rmsErrorM;
    }

    [[nodiscard]] double RmsErrorPixels() const noexcept {
        return m_rmsErrorPx;
    }

    [[nodiscard]] const std::vector<GeoControlPoint>& ControlPoints() const noexcept {
        return m_points;
    }

    /// Control points projected into the local ENU plane, in the same order.
    [[nodiscard]] const std::vector<cv::Point2d>& LocalPoints() const noexcept {
        return m_localPoints;
    }

    [[nodiscard]] std::string Describe() const;

    GeoBinding() = default;

private:
    std::vector<GeoControlPoint> m_points;
    std::vector<cv::Point2d> m_localPoints;
    cv::Matx33d m_toGround;
    cv::Matx33d m_toPixel;
    double m_originLat = 0.0;
    double m_originLon = 0.0;
    double m_rmsErrorM = 0.0;
    double m_rmsErrorPx = 0.0;
};

} // namespace tmcmt
