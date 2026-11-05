#include "trafficmcmt/edge/GeoBinding.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include "trafficmcmt/core/Geo.h"

namespace tmcmt {
namespace {

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

cv::Matx33d ToMatx33d(const cv::Mat& matrix) {
    cv::Matx33d result;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            result(row, col) = matrix.at<double>(row, col);
        }
    }
    return result;
}

cv::Point2d Apply(const cv::Matx33d& matrix, const cv::Point2d& point) {
    const double x = matrix(0, 0) * point.x + matrix(0, 1) * point.y + matrix(0, 2);
    const double y = matrix(1, 0) * point.x + matrix(1, 1) * point.y + matrix(1, 2);
    const double w = matrix(2, 0) * point.x + matrix(2, 1) * point.y + matrix(2, 2);
    return cv::Point2d(x / w, y / w);
}

} // namespace

cv::Point2d GeographicToEnu(double latitude, double longitude, double lat0, double lon0) {
    const EnuPoint enu =
        tmcmt::GeographicToEnu(GeoPoint{latitude, longitude}, GeoPoint{lat0, lon0});
    return cv::Point2d(enu.east, enu.north);
}

Result<GeoBinding> GeoBinding::LoadFromFile(const std::string& path, GeoBindingOptions options) {
    std::string error;
    const auto values = ParseIniSection(path, "points", error);
    if (!error.empty()) {
        return Status::Error(ErrorCode::IoError, error);
    }
    if (values.empty()) {
        return Status::Error(ErrorCode::ParseError, "geo binding file has no [points] section: " + path);
    }

    std::vector<GeoControlPoint> points;
    for (std::size_t index = 0;; ++index) {
        const std::string suffix = std::to_string(index);
        const std::string latKey = "lat" + suffix;
        const std::string lonKey = "lon" + suffix;
        const std::string pxKey = "px_x" + suffix;
        const std::string pyKey = "px_y" + suffix;

        const bool hasLat = values.find(latKey) != values.end();
        const bool hasLon = values.find(lonKey) != values.end();
        const bool hasPx = values.find(pxKey) != values.end();
        const bool hasPy = values.find(pyKey) != values.end();
        if (!hasLat && !hasLon && !hasPx && !hasPy) {
            break;
        }
        if (!hasLat || !hasLon || !hasPx || !hasPy) {
            return Status::Error(ErrorCode::ParseError,
                                 "incomplete geo control point " + suffix + " in " + path);
        }

        GeoControlPoint point;
        if (!ParseDouble(values, latKey, point.latitude) || !ParseDouble(values, lonKey, point.longitude) ||
            !ParseDouble(values, pxKey, point.pixel.x) || !ParseDouble(values, pyKey, point.pixel.y)) {
            return Status::Error(ErrorCode::ParseError,
                                 "invalid numeric value in geo control point " + suffix + " in " + path);
        }
        points.push_back(point);
    }

    if (points.size() < 4) {
        return Status::Error(ErrorCode::ParseError,
                             "geo binding needs at least 4 control points, got " + std::to_string(points.size()));
    }

    // Optional shared origin, used to align several cameras in one ENU frame.
    if (!options.has_origin) {
        double originLat = 0.0;
        double originLon = 0.0;
        if (ParseDouble(values, "origin_lat", originLat) && ParseDouble(values, "origin_lon", originLon)) {
            options.has_origin = true;
            options.origin_latitude = originLat;
            options.origin_longitude = originLon;
        }
    }
    return FromControlPoints(std::move(points), options);
}

Result<GeoBinding> GeoBinding::FromControlPoints(std::vector<GeoControlPoint> points, GeoBindingOptions options) {
    if (points.size() < 4) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "geo binding needs at least 4 control points, got " + std::to_string(points.size()));
    }

    GeoBinding binding;
    binding.m_points = std::move(points);

    double sumLat = 0.0;
    double sumLon = 0.0;
    for (const GeoControlPoint& point : binding.m_points) {
        if (!std::isfinite(point.latitude) || !std::isfinite(point.longitude) || !std::isfinite(point.pixel.x) ||
            !std::isfinite(point.pixel.y)) {
            return Status::Error(ErrorCode::InvalidArgument, "geo control point contains a non-finite value");
        }
        sumLat += point.latitude;
        sumLon += point.longitude;
    }

    if (options.has_origin) {
        if (!std::isfinite(options.origin_latitude) || !std::isfinite(options.origin_longitude)) {
            return Status::Error(ErrorCode::InvalidArgument, "geo binding origin is not finite");
        }
        binding.m_originLat = options.origin_latitude;
        binding.m_originLon = options.origin_longitude;
    } else {
        binding.m_originLat = sumLat / static_cast<double>(binding.m_points.size());
        binding.m_originLon = sumLon / static_cast<double>(binding.m_points.size());
    }

    std::vector<cv::Point2d> pixels;
    pixels.reserve(binding.m_points.size());
    binding.m_localPoints.reserve(binding.m_points.size());
    for (const GeoControlPoint& point : binding.m_points) {
        pixels.push_back(point.pixel);
        binding.m_localPoints.push_back(
            GeographicToEnu(point.latitude, point.longitude, binding.m_originLat, binding.m_originLon));
    }

    cv::Mat toGround = cv::findHomography(pixels, binding.m_localPoints, 0);
    cv::Mat toPixel = cv::findHomography(binding.m_localPoints, pixels, 0);
    if (toGround.empty() || toPixel.empty() || toGround.rows != 3 || toGround.cols != 3 || toPixel.rows != 3 ||
        toPixel.cols != 3) {
        return Status::Error(ErrorCode::ParseError, "cannot estimate the image-to-ground homography");
    }

    binding.m_toGround = ToMatx33d(toGround);
    binding.m_toPixel = ToMatx33d(toPixel);

    double squaredMeters = 0.0;
    double squaredPixels = 0.0;
    for (std::size_t i = 0; i < binding.m_points.size(); ++i) {
        const cv::Point2d ground = Apply(binding.m_toGround, binding.m_points[i].pixel);
        const double dx = ground.x - binding.m_localPoints[i].x;
        const double dy = ground.y - binding.m_localPoints[i].y;
        squaredMeters += dx * dx + dy * dy;

        const cv::Point2d pixel = Apply(binding.m_toPixel, binding.m_localPoints[i]);
        const double px = pixel.x - binding.m_points[i].pixel.x;
        const double py = pixel.y - binding.m_points[i].pixel.y;
        squaredPixels += px * px + py * py;
    }
    binding.m_rmsErrorM = std::sqrt(squaredMeters / static_cast<double>(binding.m_points.size()));
    binding.m_rmsErrorPx = std::sqrt(squaredPixels / static_cast<double>(binding.m_points.size()));

    if (!std::isfinite(binding.m_rmsErrorM) || !std::isfinite(binding.m_rmsErrorPx)) {
        return Status::Error(ErrorCode::ParseError, "geo binding reprojection error is not finite");
    }
    if (binding.m_rmsErrorM > options.max_rms_error_m) {
        return Status::Error(ErrorCode::ParseError,
                             "geo binding RMS error " + std::to_string(binding.m_rmsErrorM) +
                                 " m exceeds the limit of " + std::to_string(options.max_rms_error_m) + " m");
    }
    if (binding.m_rmsErrorPx > options.max_rms_error_px) {
        return Status::Error(ErrorCode::ParseError,
                             "geo binding RMS error " + std::to_string(binding.m_rmsErrorPx) +
                                 " (normalised px) exceeds the limit of " +
                                 std::to_string(options.max_rms_error_px));
    }

    return binding;
}

WorldPoint GeoBinding::ProjectNormalized(const cv::Point2d& normalized) const {
    WorldPoint world;
    if (Empty() || !std::isfinite(normalized.x) || !std::isfinite(normalized.y)) {
        return world;
    }

    const double x = m_toGround(0, 0) * normalized.x + m_toGround(0, 1) * normalized.y + m_toGround(0, 2);
    const double y = m_toGround(1, 0) * normalized.x + m_toGround(1, 1) * normalized.y + m_toGround(1, 2);
    const double w = m_toGround(2, 0) * normalized.x + m_toGround(2, 1) * normalized.y + m_toGround(2, 2);
    if (std::fabs(w) < 1e-12) {
        return world;
    }
    world.x = static_cast<float>(x / w);
    world.y = static_cast<float>(y / w);
    world.valid = std::isfinite(world.x) && std::isfinite(world.y);
    return world;
}

WorldPoint GeoBinding::Project(const cv::Point2d& pixel, cv::Size frameSize) const {
    if (frameSize.width <= 0 || frameSize.height <= 0) {
        return {};
    }
    return ProjectNormalized(
        cv::Point2d(pixel.x / static_cast<double>(frameSize.width), pixel.y / static_cast<double>(frameSize.height)));
}

WorldPoint GeoBinding::Project(const cv::Point2f& pixel, cv::Size frameSize) const {
    return Project(cv::Point2d(static_cast<double>(pixel.x), static_cast<double>(pixel.y)), frameSize);
}

Result<cv::Point2d> GeoBinding::ToNormalizedPixel(const WorldPoint& world) const {
    if (Empty() || !world.IsFinite()) {
        return Status::Error(ErrorCode::InvalidArgument, "cannot project an invalid world point");
    }
    const cv::Point2d pixel = Apply(m_toPixel, cv::Point2d(world.x, world.y));
    if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y)) {
        return Status::Error(ErrorCode::Internal, "inverse geo projection produced a non-finite point");
    }
    return pixel;
}

std::string GeoBinding::Describe() const {
    if (Empty()) {
        return "geo-binding(empty)";
    }
    std::ostringstream text;
    text << "geo-binding(points=" << m_points.size() << ", origin=" << m_originLat << "," << m_originLon
         << ", rms=" << m_rmsErrorM << "m/" << m_rmsErrorPx << "px)";
    return text.str();
}

} // namespace tmcmt
