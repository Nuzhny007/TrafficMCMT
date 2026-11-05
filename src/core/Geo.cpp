#include "trafficmcmt/core/Geo.h"

#include <cmath>

namespace tmcmt {
namespace {

constexpr double kDegToRad = 0.017453292519943295769236907684886;
constexpr double kRadToDeg = 57.295779513082320876798154814105;
constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84F = 1.0 / 298.257223563;

struct Ecef {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Ecef ToEcef(const GeoPoint& point) noexcept {
    const double e2 = kWgs84F * (2.0 - kWgs84F);
    const double lat = point.latitude * kDegToRad;
    const double lon = point.longitude * kDegToRad;
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double n = kWgs84A / std::sqrt(1.0 - e2 * sinLat * sinLat);

    Ecef ecef;
    ecef.x = n * cosLat * std::cos(lon);
    ecef.y = n * cosLat * std::sin(lon);
    ecef.z = n * (1.0 - e2) * sinLat;
    return ecef;
}

} // namespace

EnuPoint GeographicToEnu(const GeoPoint& point, const GeoPoint& origin) noexcept {
    if (!point.IsFinite() || !origin.IsFinite()) {
        return {};
    }

    const Ecef p = ToEcef(point);
    const Ecef o = ToEcef(origin);

    const double sinLat0 = std::sin(origin.latitude * kDegToRad);
    const double cosLat0 = std::cos(origin.latitude * kDegToRad);
    const double sinLon0 = std::sin(origin.longitude * kDegToRad);
    const double cosLon0 = std::cos(origin.longitude * kDegToRad);

    const double dx = p.x - o.x;
    const double dy = p.y - o.y;
    const double dz = p.z - o.z;

    EnuPoint enu;
    enu.east = -sinLon0 * dx + cosLon0 * dy;
    enu.north = -sinLat0 * cosLon0 * dx - sinLat0 * sinLon0 * dy + cosLat0 * dz;
    return enu;
}

GeoPoint EnuToGeographic(const EnuPoint& point, const GeoPoint& origin) noexcept {
    if (!point.IsFinite() || !origin.IsFinite()) {
        return {};
    }

    const double e2 = kWgs84F * (2.0 - kWgs84F);
    const double sinLat0 = std::sin(origin.latitude * kDegToRad);
    const double cosLat0 = std::cos(origin.latitude * kDegToRad);
    const double sinLon0 = std::sin(origin.longitude * kDegToRad);
    const double cosLon0 = std::cos(origin.longitude * kDegToRad);

    const Ecef o = ToEcef(origin);

    // Inverse rotation of the ENU frame (up = 0).
    const double dx = -sinLon0 * point.east - sinLat0 * cosLon0 * point.north;
    const double dy = cosLon0 * point.east - sinLat0 * sinLon0 * point.north;
    const double dz = cosLat0 * point.north;

    const double x = o.x + dx;
    const double y = o.y + dy;
    const double z = o.z + dz;

    const double longitude = std::atan2(y, x);
    const double p = std::hypot(x, y);

    // Iterative geodetic latitude. Five iterations are enough for centimetre
    // accuracy anywhere outside the polar caps.
    double latitude = std::atan2(z, p * (1.0 - e2));
    for (int i = 0; i < 6; ++i) {
        const double sinLat = std::sin(latitude);
        const double n = kWgs84A / std::sqrt(1.0 - e2 * sinLat * sinLat);
        const double h = (std::fabs(std::cos(latitude)) > 1e-12) ? p / std::cos(latitude) - n : 0.0;
        const double next = std::atan2(z, p * (1.0 - e2 * n / (n + h)));
        if (std::fabs(next - latitude) < 1e-14) {
            latitude = next;
            break;
        }
        latitude = next;
    }

    GeoPoint result;
    result.latitude = latitude * kRadToDeg;
    result.longitude = longitude * kRadToDeg;
    return result;
}

} // namespace tmcmt
