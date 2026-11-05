#include <gtest/gtest.h>

#include <cmath>

#include "trafficmcmt/core/Geo.h"

namespace {

using tmcmt::EnuPoint;
using tmcmt::EnuToGeographic;
using tmcmt::GeographicToEnu;
using tmcmt::GeoPoint;

constexpr double kOriginLat = 32.606456;
constexpr double kOriginLon = -85.481891;

TEST(Geo, OriginMapsToZero) {
    const GeoPoint origin{kOriginLat, kOriginLon};
    const EnuPoint enu = GeographicToEnu(origin, origin);
    EXPECT_NEAR(enu.east, 0.0, 1e-6);
    EXPECT_NEAR(enu.north, 0.0, 1e-6);
}

TEST(Geo, EnuToGeographicRoundTrip) {
    const GeoPoint origin{kOriginLat, kOriginLon};
    const EnuPoint points[] = {{0.0, 0.0}, {100.0, -250.0}, {-1234.5, 987.25}, {1e-3, -1e-3}};
    for (const EnuPoint& point : points) {
        const GeoPoint geo = EnuToGeographic(point, origin);
        const EnuPoint back = GeographicToEnu(geo, origin);
        EXPECT_NEAR(back.east, point.east, 1e-3);
        EXPECT_NEAR(back.north, point.north, 1e-3);
    }
}

TEST(Geo, OneMillidegreeLatitudeIsAboutOneHundredElevenMetres) {
    const GeoPoint origin{kOriginLat, kOriginLon};
    const EnuPoint enu = GeographicToEnu(GeoPoint{kOriginLat + 0.001, kOriginLon}, origin);
    EXPECT_NEAR(enu.east, 0.0, 1e-6);
    EXPECT_GT(enu.north, 110.0);
    EXPECT_LT(enu.north, 112.0);
}

TEST(Geo, OneMillidegreeLongitudeIsScaledByCosineLatitude) {
    const GeoPoint origin{kOriginLat, kOriginLon};
    const EnuPoint enu = GeographicToEnu(GeoPoint{kOriginLat, kOriginLon + 0.001}, origin);
    const double expected = 111.32 * std::cos(kOriginLat * 3.14159265358979323846 / 180.0);
    EXPECT_NEAR(enu.north, 0.0, 1e-2);
    EXPECT_NEAR(enu.east, expected, 0.5);
}

TEST(Geo, NonFiniteInputReturnsZero) {
    const GeoPoint origin{kOriginLat, kOriginLon};
    const GeoPoint nan{std::nan(""), kOriginLon};
    const EnuPoint a = GeographicToEnu(nan, origin);
    EXPECT_EQ(a.east, 0.0);
    EXPECT_EQ(a.north, 0.0);

    const GeoPoint bad = EnuToGeographic(EnuPoint{std::nan(""), 1.0}, origin);
    EXPECT_EQ(bad.latitude, 0.0);
    EXPECT_EQ(bad.longitude, 0.0);
}

} // namespace
