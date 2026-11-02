#pragma once

#include <cstdint>
#include <string>

#include <opencv2/core.hpp>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt {

/// Extracts an appearance descriptor for a bounding box.
///
/// Implementations live on the Edge node: the Master never sees images. The
/// interface is deliberately independent of Multitarget-tracker so that a
/// production Edge can plug in a DNN model, while tests and CPU-only builds
/// use `ColorHistogramReIdExtractor`.
class IReIdExtractor {
public:
    virtual ~IReIdExtractor() = default;

    /// `bgr` is an 8-bit 3-channel image; `roi` is in image coordinates.
    [[nodiscard]] virtual Result<Embedding> Extract(const cv::Mat& bgr, const cv::Rect& roi) const = 0;

    [[nodiscard]] virtual int Dim() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t ModelId() const noexcept = 0;
    [[nodiscard]] virtual std::string Describe() const = 0;
};

} // namespace tmcmt
