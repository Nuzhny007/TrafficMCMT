#include "trafficmcmt/st/TransitionEstimator.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace tmcmt::st {
namespace {

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle),
                     values.end());
    const double upper = values[middle];
    if (values.size() % 2 != 0) {
        return upper;
    }
    const auto lowerIt = std::max_element(values.begin(),
                                          values.begin() + static_cast<std::ptrdiff_t>(middle));
    return 0.5 * (*lowerIt + upper);
}

double MedianAbsoluteDeviation(const std::vector<double>& values, double center) {
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const double value : values) {
        deviations.push_back(std::abs(value - center));
    }
    return Median(std::move(deviations));
}

struct GroupEstimate {
    double mean = 0.0;
    double sigma = 0.0;
    std::size_t samples = 0;
};

GroupEstimate EstimateGroup(std::vector<double> values, const EstimatorOptions& options) {
    const double center = Median(values);
    const double mad = MedianAbsoluteDeviation(values, center);
    double sigma = options.mad_scale * mad;
    std::size_t used = values.size();

    if (options.outlier_sigma > 0.0 && sigma > 0.0) {
        const double limit = options.outlier_sigma * sigma;
        std::vector<double> trimmed;
        trimmed.reserve(values.size());
        for (const double value : values) {
            if (std::abs(value - center) <= limit) {
                trimmed.push_back(value);
            }
        }
        if (trimmed.size() >= options.min_samples) {
            const double trimmedCenter = Median(trimmed);
            const double trimmedMad = MedianAbsoluteDeviation(trimmed, trimmedCenter);
            values = std::move(trimmed);
            used = values.size();
            const double trimmedSigma = options.mad_scale * trimmedMad;
            return GroupEstimate{trimmedCenter, std::max(trimmedSigma, options.min_stddev_ms), used};
        }
    }

    return GroupEstimate{center, std::max(sigma, options.min_stddev_ms), used};
}

} // namespace

Result<std::vector<CameraLink>> TransitionEstimator::Estimate(
    std::span<const TransitionSample> samples, const EstimatorOptions& options) {
    if (options.min_samples == 0) {
        return Status::Error(ErrorCode::InvalidArgument, "min_samples must be > 0");
    }
    if (!(options.mad_scale > 0.0)) {
        return Status::Error(ErrorCode::InvalidArgument, "mad_scale must be > 0");
    }
    if (!(options.min_stddev_ms > 0.0)) {
        return Status::Error(ErrorCode::InvalidArgument, "min_stddev_ms must be > 0");
    }
    if (options.outlier_sigma < 0.0) {
        return Status::Error(ErrorCode::InvalidArgument, "outlier_sigma must be >= 0");
    }

    using LinkKey = std::pair<CameraId, CameraId>;
    std::map<LinkKey, std::vector<double>> groups;
    for (const TransitionSample& sample : samples) {
        if (sample.from.empty() || sample.to.empty()) {
            continue;
        }
        if (!options.include_same_camera && sample.from == sample.to) {
            continue;
        }
        const std::int64_t delta = (sample.to_begin - sample.from_end).count();
        if (delta < 0) {
            continue;
        }
        groups[LinkKey{sample.from, sample.to}].push_back(static_cast<double>(delta));
    }

    std::vector<CameraLink> links;
    links.reserve(groups.size());
    for (auto& [key, values] : groups) {
        if (values.size() < options.min_samples) {
            continue;
        }
        const GroupEstimate estimate = EstimateGroup(std::move(values), options);
        CameraLink link;
        link.from = key.first;
        link.to = key.second;
        link.transition = TransitionStats{estimate.mean, estimate.sigma,
                                          static_cast<std::int64_t>(estimate.samples)};
        link.enabled = true;
        links.push_back(std::move(link));
    }
    return links;
}

} // namespace tmcmt::st
