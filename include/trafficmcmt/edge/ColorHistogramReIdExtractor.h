#pragma once

#include <cstdint>

#include "trafficmcmt/edge/IReIdExtractor.h"

namespace tmcmt {

struct ColorHistogramOptions {
    /// Bins per HSV channel. The descriptor dimension is bins^3.
    int bins = 8;
    std::uint32_t model_id = 1;
    int min_roi_size = 8;
};

/// Dependency-free ReID baseline: a joint HSV histogram, L2-normalised.
///
/// This is not a competitive ReID model; it exists so that the whole
/// Edge -> file -> Master path can be exercised without downloading weights,
/// and as a deterministic reference for tests.
class ColorHistogramReIdExtractor final : public IReIdExtractor {
public:
    explicit ColorHistogramReIdExtractor(ColorHistogramOptions options = {});

    [[nodiscard]] Result<Embedding> Extract(const cv::Mat& bgr, const cv::Rect& roi) const override;

    [[nodiscard]] int Dim() const noexcept override {
        return m_dim;
    }

    [[nodiscard]] std::uint32_t ModelId() const noexcept override {
        return m_options.model_id;
    }

    [[nodiscard]] std::string Describe() const override;

private:
    ColorHistogramOptions m_options;
    int m_dim = 0;
};

} // namespace tmcmt
