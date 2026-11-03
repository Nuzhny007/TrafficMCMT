#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "trafficmcmt/edge/GeoBinding.h"

namespace {

using tmcmt::ErrorCode;
using tmcmt::GeoBinding;
using tmcmt::GeoBindingOptions;
using tmcmt::GeoControlPoint;

std::vector<GeoControlPoint> MakeSquareControlPoints() {
    return {
        {45.000, 5.000, {0.0, 0.0}},
        {45.000, 5.001, {1.0, 0.0}},
        {45.001, 5.001, {1.0, 1.0}},
        {45.001, 5.000, {0.0, 1.0}},
    };
}

std::filesystem::path WriteTempIni(const std::string& name, const std::string& content) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream stream(path, std::ios::trunc);
    stream << content;
    return path;
}

TEST(GeoBinding, ConvertsWgs84ToLocalEnu) {
    const cv::Point2d origin = tmcmt::GeographicToEnu(45.0, 5.0, 45.0, 5.0);
    EXPECT_NEAR(origin.x, 0.0, 1e-6);
    EXPECT_NEAR(origin.y, 0.0, 1e-6);

    const cv::Point2d north = tmcmt::GeographicToEnu(45.001, 5.0, 45.0, 5.0);
    EXPECT_NEAR(north.x, 0.0, 0.01);
    EXPECT_NEAR(north.y, 111.13, 0.5);

    const cv::Point2d east = tmcmt::GeographicToEnu(45.0, 5.001, 45.0, 5.0);
    EXPECT_NEAR(east.x, 78.85, 0.5);
    EXPECT_NEAR(east.y, 0.0, 0.01);
}

TEST(GeoBinding, ProjectsControlPointsExactly) {
    auto binding = GeoBinding::FromControlPoints(MakeSquareControlPoints());
    ASSERT_TRUE(binding) << binding.status().ToString();
    EXPECT_FALSE(binding.value().Empty());
    EXPECT_EQ(binding.value().ControlPointCount(), 4U);
    EXPECT_NEAR(binding.value().OriginLatitude(), 45.0005, 1e-9);
    EXPECT_NEAR(binding.value().OriginLongitude(), 5.0005, 1e-9);
    EXPECT_LT(binding.value().RmsErrorMeters(), 1e-4);
    EXPECT_LT(binding.value().RmsErrorPixels(), 1e-6);

    const auto& points = binding.value().ControlPoints();
    const auto& local = binding.value().LocalPoints();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const tmcmt::WorldPoint world = binding.value().ProjectNormalized(points[i].pixel);
        ASSERT_TRUE(world.valid);
        EXPECT_NEAR(world.x, local[i].x, 1e-5);
        EXPECT_NEAR(world.y, local[i].y, 1e-5);
    }
}

TEST(GeoBinding, PixelAndNormalizedProjectionAgree) {
    auto binding = GeoBinding::FromControlPoints(MakeSquareControlPoints());
    ASSERT_TRUE(binding);

    const cv::Size frameSize(1280, 720);
    const tmcmt::WorldPoint fromPixels = binding.value().Project(cv::Point2d(640.0, 360.0), frameSize);
    const tmcmt::WorldPoint fromNormalized = binding.value().ProjectNormalized(cv::Point2d(0.5, 0.5));
    ASSERT_TRUE(fromPixels.valid);
    ASSERT_TRUE(fromNormalized.valid);
    EXPECT_NEAR(fromPixels.x, fromNormalized.x, 1e-4);
    EXPECT_NEAR(fromPixels.y, fromNormalized.y, 1e-4);
}

TEST(GeoBinding, InverseProjectionRoundTrips) {
    auto binding = GeoBinding::FromControlPoints(MakeSquareControlPoints());
    ASSERT_TRUE(binding);

    for (const GeoControlPoint& point : binding.value().ControlPoints()) {
        const tmcmt::WorldPoint world = binding.value().ProjectNormalized(point.pixel);
        ASSERT_TRUE(world.valid);
        auto pixel = binding.value().ToNormalizedPixel(world);
        ASSERT_TRUE(pixel) << pixel.status().ToString();
        EXPECT_NEAR(pixel.value().x, point.pixel.x, 1e-6);
        EXPECT_NEAR(pixel.value().y, point.pixel.y, 1e-6);
    }
}

TEST(GeoBinding, ParsesIniFile) {
    const std::string content =
        "# comment\n"
        "[points]\n"
        "lat0 = 45.000\n"
        "lon0 = 5.000\n"
        "px_x0 = 0.0\n"
        "px_y0 = 0.0\n"
        "\n"
        "lat1 = 45.000\n"
        "lon1 = 5.001\n"
        "px_x1 = 1.0\n"
        "px_y1 = 0.0\n"
        "\n"
        "lat2 = 45.001\n"
        "lon2 = 5.001\n"
        "px_x2 = 1.0\n"
        "px_y2 = 1.0\n"
        "\n"
        "lat3 = 45.001\n"
        "lon3 = 5.000\n"
        "px_x3 = 0.0\n"
        "px_y3 = 1.0\n"
        "\n"
        "[lines]\n"
        "line0_x0 = 0.1\n"
        "line0_y0 = 0.2\n";

    const std::filesystem::path path = WriteTempIni("tmcmt_geo_binding_valid.ini", content);
    auto binding = GeoBinding::LoadFromFile(path.string());
    std::filesystem::remove(path);

    ASSERT_TRUE(binding) << binding.status().ToString();
    EXPECT_EQ(binding.value().ControlPointCount(), 4U);
    EXPECT_LT(binding.value().RmsErrorMeters(), 1e-4);
}

TEST(GeoBinding, RejectsMissingFile) {
    auto binding = GeoBinding::LoadFromFile("this/file/does/not/exist.ini");
    EXPECT_FALSE(binding);
    EXPECT_EQ(binding.status().code(), ErrorCode::IoError);
}

TEST(GeoBinding, RejectsIncompleteControlPoint) {
    const std::string content =
        "[points]\n"
        "lat0 = 45.0\n"
        "lon0 = 5.0\n"
        "px_x0 = 0.0\n"
        "px_y0 = 0.0\n"
        "lat1 = 45.0\n"
        "lon1 = 5.001\n"
        "px_x1 = 1.0\n";

    const std::filesystem::path path = WriteTempIni("tmcmt_geo_binding_incomplete.ini", content);
    auto binding = GeoBinding::LoadFromFile(path.string());
    std::filesystem::remove(path);

    EXPECT_FALSE(binding);
    EXPECT_EQ(binding.status().code(), ErrorCode::ParseError);
}

TEST(GeoBinding, RejectsTooFewPoints) {
    std::vector<GeoControlPoint> points = MakeSquareControlPoints();
    points.pop_back();

    auto binding = GeoBinding::FromControlPoints(points);
    EXPECT_FALSE(binding);
    EXPECT_EQ(binding.status().code(), ErrorCode::InvalidArgument);
}

TEST(GeoBinding, RejectsLargeReprojectionError) {
    std::vector<GeoControlPoint> points = MakeSquareControlPoints();
    // A fifth correspondence that contradicts the other four.
    points.push_back({45.0005, 5.0005, {0.9, 0.1}});

    GeoBindingOptions options;
    options.max_rms_error_m = 1e-3;
    auto binding = GeoBinding::FromControlPoints(points, options);
    EXPECT_FALSE(binding);
    EXPECT_EQ(binding.status().code(), ErrorCode::ParseError);
}

TEST(GeoBinding, UsesExplicitSharedOrigin) {
    GeoBindingOptions options;
    options.has_origin = true;
    options.origin_latitude = 45.0;
    options.origin_longitude = 5.0;

    auto binding = GeoBinding::FromControlPoints(MakeSquareControlPoints(), options);
    ASSERT_TRUE(binding) << binding.status().ToString();
    EXPECT_NEAR(binding.value().OriginLatitude(), 45.0, 1e-12);
    EXPECT_NEAR(binding.value().OriginLongitude(), 5.0, 1e-12);

    // The south-west control point sits exactly at the requested origin.
    const auto& local = binding.value().LocalPoints();
    ASSERT_EQ(local.size(), 4U);
    EXPECT_NEAR(local[0].x, 0.0, 1e-6);
    EXPECT_NEAR(local[0].y, 0.0, 1e-6);
}

TEST(GeoBinding, ParsesOriginFromIni) {
    const std::string content =
        "[points]\n"
        "origin_lat = 45.0\n"
        "origin_lon = 5.0\n"
        "lat0 = 45.000\n"
        "lon0 = 5.000\n"
        "px_x0 = 0.0\n"
        "px_y0 = 0.0\n"
        "lat1 = 45.000\n"
        "lon1 = 5.001\n"
        "px_x1 = 1.0\n"
        "px_y1 = 0.0\n"
        "lat2 = 45.001\n"
        "lon2 = 5.001\n"
        "px_x2 = 1.0\n"
        "px_y2 = 1.0\n"
        "lat3 = 45.001\n"
        "lon3 = 5.000\n"
        "px_x3 = 0.0\n"
        "px_y3 = 1.0\n";

    const std::filesystem::path path = WriteTempIni("tmcmt_geo_binding_origin.ini", content);
    auto binding = GeoBinding::LoadFromFile(path.string());
    std::filesystem::remove(path);

    ASSERT_TRUE(binding) << binding.status().ToString();
    EXPECT_NEAR(binding.value().OriginLatitude(), 45.0, 1e-12);
    EXPECT_NEAR(binding.value().OriginLongitude(), 5.0, 1e-12);
}

} // namespace
