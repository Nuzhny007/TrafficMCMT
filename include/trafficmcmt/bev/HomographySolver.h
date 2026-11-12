#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "trafficmcmt/core/Camera.h"
#include "trafficmcmt/core/Result.h"

namespace tmcmt::bev {

using Correspondence = std::array<double, 2>;

struct HomographyFit {
    Homography homography{};
    /// RMS reprojection error in ground-plane metres.
    double rms_error_m = 0.0;
    std::size_t inliers = 0;
};

/// Direct linear transform for image -> ground homographies.
///
/// The solver normalises both point sets (Hartley) before building the linear
/// system, so it is stable for pixel coordinates in the thousands. An optional
/// deterministic RANSAC pass rejects outliers. No OpenCV dependency.
class HomographySolver {
public:
    /// Requires at least four correspondences. `max_error_m` is the RANSAC
    /// inlier threshold; it is ignored when `ransac` is false.
    [[nodiscard]] static Result<HomographyFit> Fit(const std::vector<Correspondence>& image,
                                                   const std::vector<Correspondence>& ground,
                                                   bool ransac = false,
                                                   double max_error_m = 1.0);

    /// Convenience wrapper returning only the matrix.
    [[nodiscard]] static Result<Homography> FromCorrespondences(
        const std::vector<Correspondence>& image, const std::vector<Correspondence>& ground,
        bool ransac = false, double max_error_m = 1.0);
};

} // namespace tmcmt::bev
