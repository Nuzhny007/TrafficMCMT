#include "trafficmcmt/bev/HomographySolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace tmcmt::bev {
namespace {

constexpr double kEpsilon = 1e-12;
constexpr int kJacobiIterations = 100;

struct Normalization {
    double cx = 0.0;
    double cy = 0.0;
    double scale = 1.0;
};

Normalization ComputeNormalization(const std::vector<Correspondence>& points) {
    Normalization out;
    if (points.empty()) {
        return out;
    }
    for (const Correspondence& point : points) {
        out.cx += point[0];
        out.cy += point[1];
    }
    const double n = static_cast<double>(points.size());
    out.cx /= n;
    out.cy /= n;

    double meanDistance = 0.0;
    for (const Correspondence& point : points) {
        meanDistance += std::hypot(point[0] - out.cx, point[1] - out.cy);
    }
    meanDistance /= n;
    out.scale = meanDistance > kEpsilon ? std::sqrt(2.0) / meanDistance : 1.0;
    return out;
}

std::array<double, 2> NormalizePoint(const Normalization& normalization, double x, double y) {
    return {(x - normalization.cx) * normalization.scale, (y - normalization.cy) * normalization.scale};
}

void Multiply3x3(const std::array<double, 9>& a, const std::array<double, 9>& b,
                 std::array<double, 9>& out) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[static_cast<std::size_t>(row * 3 + col)] =
                a[static_cast<std::size_t>(row * 3 + 0)] * b[static_cast<std::size_t>(0 * 3 + col)] +
                a[static_cast<std::size_t>(row * 3 + 1)] * b[static_cast<std::size_t>(1 * 3 + col)] +
                a[static_cast<std::size_t>(row * 3 + 2)] * b[static_cast<std::size_t>(2 * 3 + col)];
        }
    }
}

/// Jacobi eigen decomposition of a symmetric 9x9 matrix.
void Jacobi(std::array<std::array<double, 9>, 9>& a, std::array<std::array<double, 9>, 9>& v) {
    for (auto& row : v) {
        row.fill(0.0);
    }
    for (int i = 0; i < 9; ++i) {
        v[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] = 1.0;
    }

    for (int iteration = 0; iteration < kJacobiIterations; ++iteration) {
        int p = 0;
        int q = 1;
        double largest = 0.0;
        for (int row = 0; row < 9; ++row) {
            for (int col = row + 1; col < 9; ++col) {
                const double value = std::fabs(a[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)]);
                if (value > largest) {
                    largest = value;
                    p = row;
                    q = col;
                }
            }
        }
        if (largest < 1e-14) {
            break;
        }

        const double app = a[static_cast<std::size_t>(p)][static_cast<std::size_t>(p)];
        const double aqq = a[static_cast<std::size_t>(q)][static_cast<std::size_t>(q)];
        const double apq = a[static_cast<std::size_t>(p)][static_cast<std::size_t>(q)];
        const double tau = (aqq - app) / (2.0 * apq);
        const double t = (tau >= 0.0 ? 1.0 : -1.0) / (std::fabs(tau) + std::sqrt(1.0 + tau * tau));
        const double c = 1.0 / std::sqrt(1.0 + t * t);
        const double s = t * c;

        for (int k = 0; k < 9; ++k) {
            if (k == p || k == q) {
                continue;
            }
            const double akp = a[static_cast<std::size_t>(k)][static_cast<std::size_t>(p)];
            const double akq = a[static_cast<std::size_t>(k)][static_cast<std::size_t>(q)];
            a[static_cast<std::size_t>(k)][static_cast<std::size_t>(p)] = c * akp - s * akq;
            a[static_cast<std::size_t>(k)][static_cast<std::size_t>(q)] = s * akp + c * akq;
            a[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)] =
                a[static_cast<std::size_t>(k)][static_cast<std::size_t>(p)];
            a[static_cast<std::size_t>(q)][static_cast<std::size_t>(k)] =
                a[static_cast<std::size_t>(k)][static_cast<std::size_t>(q)];
        }
        a[static_cast<std::size_t>(p)][static_cast<std::size_t>(p)] =
            c * c * app - 2.0 * s * c * apq + s * s * aqq;
        a[static_cast<std::size_t>(q)][static_cast<std::size_t>(q)] =
            s * s * app + 2.0 * s * c * apq + c * c * aqq;
        a[static_cast<std::size_t>(p)][static_cast<std::size_t>(q)] = 0.0;
        a[static_cast<std::size_t>(q)][static_cast<std::size_t>(p)] = 0.0;

        for (int k = 0; k < 9; ++k) {
            const double vkp = v[static_cast<std::size_t>(k)][static_cast<std::size_t>(p)];
            const double vkq = v[static_cast<std::size_t>(k)][static_cast<std::size_t>(q)];
            v[static_cast<std::size_t>(k)][static_cast<std::size_t>(p)] = c * vkp - s * vkq;
            v[static_cast<std::size_t>(k)][static_cast<std::size_t>(q)] = s * vkp + c * vkq;
        }
    }
}

Result<Homography> FitDlt(const std::vector<Correspondence>& image,
                          const std::vector<Correspondence>& ground) {
    const Normalization imageNorm = ComputeNormalization(image);
    const Normalization groundNorm = ComputeNormalization(ground);

    std::array<std::array<double, 9>, 9> ata{};
    for (std::size_t i = 0; i < image.size(); ++i) {
        const auto [u, v] = NormalizePoint(imageNorm, image[i][0], image[i][1]);
        const auto [x, y] = NormalizePoint(groundNorm, ground[i][0], ground[i][1]);

        const std::array<double, 9> row1{-u, -v, -1.0, 0.0, 0.0, 0.0, u * x, v * x, x};
        const std::array<double, 9> row2{0.0, 0.0, 0.0, -u, -v, -1.0, u * y, v * y, y};
        for (int r = 0; r < 9; ++r) {
            for (int c = 0; c < 9; ++c) {
                ata[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] +=
                    row1[static_cast<std::size_t>(r)] * row1[static_cast<std::size_t>(c)] +
                    row2[static_cast<std::size_t>(r)] * row2[static_cast<std::size_t>(c)];
            }
        }
    }

    std::array<std::array<double, 9>, 9> eigenvectors{};
    Jacobi(ata, eigenvectors);

    int smallest = 0;
    double smallestValue = ata[0][0];
    for (int i = 1; i < 9; ++i) {
        if (ata[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] < smallestValue) {
            smallestValue = ata[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)];
            smallest = i;
        }
    }

    std::array<double, 9> hn{};
    for (int row = 0; row < 9; ++row) {
        hn[static_cast<std::size_t>(row)] =
            eigenvectors[static_cast<std::size_t>(row)][static_cast<std::size_t>(smallest)];
    }

    // H = S^-1 * Hn * T.
    const std::array<double, 9> t{imageNorm.scale, 0.0, -imageNorm.scale * imageNorm.cx,
                                  0.0, imageNorm.scale, -imageNorm.scale * imageNorm.cy,
                                  0.0, 0.0, 1.0};
    const std::array<double, 9> sInverse{1.0 / groundNorm.scale, 0.0, groundNorm.cx,
                                         0.0, 1.0 / groundNorm.scale, groundNorm.cy,
                                         0.0, 0.0, 1.0};

    std::array<double, 9> tmp{};
    std::array<double, 9> h{};
    Multiply3x3(hn, t, tmp);
    Multiply3x3(sInverse, tmp, h);

    double norm = 0.0;
    for (const double value : h) {
        norm += value * value;
    }
    if (!(norm > kEpsilon) || !std::isfinite(norm)) {
        return Status::Error(ErrorCode::InvalidArgument, "homography estimation is degenerate");
    }
    if (std::fabs(h[8]) > 1e-9) {
        const double scale = 1.0 / h[8];
        for (double& value : h) {
            value *= scale;
        }
    } else {
        const double scale = 1.0 / std::sqrt(norm);
        for (double& value : h) {
            value *= scale;
        }
    }
    for (const double value : h) {
        if (!std::isfinite(value)) {
            return Status::Error(ErrorCode::InvalidArgument, "homography contains a non-finite value");
        }
    }
    return h;
}

double ReprojectionError(const Homography& h, const Correspondence& image, const Correspondence& ground) {
    const double denominator = h[6] * image[0] + h[7] * image[1] + h[8];
    if (!std::isfinite(denominator) || std::fabs(denominator) < 1e-9) {
        return std::numeric_limits<double>::infinity();
    }
    const double x = (h[0] * image[0] + h[1] * image[1] + h[2]) / denominator;
    const double y = (h[3] * image[0] + h[4] * image[1] + h[5]) / denominator;
    return std::hypot(x - ground[0], y - ground[1]);
}

double Rms(const Homography& h, const std::vector<Correspondence>& image,
           const std::vector<Correspondence>& ground) {
    if (image.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < image.size(); ++i) {
        const double error = ReprojectionError(h, image[i], ground[i]);
        if (!std::isfinite(error)) {
            return std::numeric_limits<double>::infinity();
        }
        sum += error * error;
    }
    return std::sqrt(sum / static_cast<double>(image.size()));
}

} // namespace

Result<HomographyFit> HomographySolver::Fit(const std::vector<Correspondence>& image,
                                            const std::vector<Correspondence>& ground, bool ransac,
                                            double max_error_m) {
    if (image.size() != ground.size()) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "image and ground correspondence counts differ");
    }
    if (image.size() < 4) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "at least four correspondences are required");
    }
    for (std::size_t i = 0; i < image.size(); ++i) {
        if (!std::isfinite(image[i][0]) || !std::isfinite(image[i][1]) || !std::isfinite(ground[i][0]) ||
            !std::isfinite(ground[i][1])) {
            return Status::Error(ErrorCode::InvalidArgument,
                                 "correspondence " + std::to_string(i) + " is not finite");
        }
    }

    const double threshold = max_error_m > 0.0 ? max_error_m : 1.0;

    if (!ransac || image.size() == 4) {
        auto h = FitDlt(image, ground);
        if (!h) {
            return h.status();
        }
        return HomographyFit{h.value(), Rms(h.value(), image, ground), image.size()};
    }

    std::mt19937 rng(0x5EEDU);
    std::uniform_int_distribution<std::size_t> distribution(0, image.size() - 1);
    std::size_t bestInliers = 0;
    double bestRms = std::numeric_limits<double>::infinity();
    Homography best{};
    bool haveBest = false;

    const int iterations = std::min(500, std::max(50, static_cast<int>(image.size()) * 20));
    for (int iteration = 0; iteration < iterations; ++iteration) {
        std::array<std::size_t, 4> indices{};
        std::size_t count = 0;
        while (count < indices.size()) {
            const std::size_t candidate = distribution(rng);
            bool duplicate = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (indices[i] == candidate) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                indices[count++] = candidate;
            }
        }

        std::vector<Correspondence> sampleImage;
        std::vector<Correspondence> sampleGround;
        sampleImage.reserve(4);
        sampleGround.reserve(4);
        for (const std::size_t index : indices) {
            sampleImage.push_back(image[index]);
            sampleGround.push_back(ground[index]);
        }

        auto candidate = FitDlt(sampleImage, sampleGround);
        if (!candidate) {
            continue;
        }

        std::size_t inliers = 0;
        for (std::size_t i = 0; i < image.size(); ++i) {
            if (ReprojectionError(candidate.value(), image[i], ground[i]) <= threshold) {
                ++inliers;
            }
        }
        if (inliers > bestInliers) {
            bestInliers = inliers;
            best = candidate.value();
            haveBest = true;
            bestRms = Rms(best, image, ground);
        }
    }

    if (!haveBest || bestInliers < 4) {
        auto h = FitDlt(image, ground);
        if (!h) {
            return h.status();
        }
        return HomographyFit{h.value(), Rms(h.value(), image, ground), image.size()};
    }

    // Refit on the best inlier set.
    std::vector<Correspondence> inlierImage;
    std::vector<Correspondence> inlierGround;
    for (std::size_t i = 0; i < image.size(); ++i) {
        if (ReprojectionError(best, image[i], ground[i]) <= threshold) {
            inlierImage.push_back(image[i]);
            inlierGround.push_back(ground[i]);
        }
    }
    if (inlierImage.size() >= 4) {
        auto refined = FitDlt(inlierImage, inlierGround);
        if (refined) {
            best = refined.value();
            bestRms = Rms(best, inlierImage, inlierGround);
            bestInliers = inlierImage.size();
        }
    }
    return HomographyFit{best, bestRms, bestInliers};
}

Result<Homography> HomographySolver::FromCorrespondences(const std::vector<Correspondence>& image,
                                                         const std::vector<Correspondence>& ground,
                                                         bool ransac, double max_error_m) {
    auto fit = Fit(image, ground, ransac, max_error_m);
    if (!fit) {
        return fit.status();
    }
    return fit.value().homography;
}

} // namespace tmcmt::bev
