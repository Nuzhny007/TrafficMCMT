#pragma once

#include <cmath>

namespace tmcmt {

/// WGS-84 geographic point in degrees.
struct GeoPoint {
    double latitude = 0.0;
    double longitude = 0.0;

    [[nodiscard]] bool IsFinite() const noexcept {
        return std::isfinite(latitude) && std::isfinite(longitude);
    }
};

/// Local East-North-Up plane in metres.
struct EnuPoint {
    double east = 0.0;
    double north = 0.0;

    [[nodiscard]] bool IsFinite() const noexcept {
        return std::isfinite(east) && std::isfinite(north);
    }
};

/// WGS-84 latitude/longitude -> local ENU metres.
///
/// This is the single source of truth for the metric frame shared by the Edge
/// exporter, the Master and the map feed. It mirrors the WGS84 ellipsoid
/// formulas used by Multitarget-tracker's `LocalPlane`, so all projects agree
/// on world coordinates.
[[nodiscard]] EnuPoint GeographicToEnu(const GeoPoint& point, const GeoPoint& origin) noexcept;

/// Local ENU metres -> WGS-84 latitude/longitude. Exact inverse of
/// `GeographicToEnu` up to floating-point precision.
[[nodiscard]] GeoPoint EnuToGeographic(const EnuPoint& point, const GeoPoint& origin) noexcept;

} // namespace tmcmt
