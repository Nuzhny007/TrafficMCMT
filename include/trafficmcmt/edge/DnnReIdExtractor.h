#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/dnn.hpp>
#include <opencv2/core.hpp>

#include "trafficmcmt/edge/IReIdExtractor.h"

namespace tmcmt {

struct DnnReIdOptions {
    /// Path to an ONNX model whose output is a 1-D feature vector.
    std::string model_path;
    cv::Size input_size{128, 256};
    std::uint32_t model_id = 2;
    cv::Scalar mean{0.0, 0.0, 0.0};
    /// Per-channel standard deviation applied after `scale` and channel swap.
    cv::Scalar stddev{1.0, 1.0, 1.0};
    double scale = 1.0 / 255.0;
    bool swap_rb = true;
    int min_roi_size = 8;
};

/// ReID extractor backed by an OpenCV DNN model (ONNX).
///
/// The output of the network is flattened and L2-normalised. The model is
/// loaded once in `Create`; `Extract` is safe to call from a single thread.
class DnnReIdExtractor final : public IReIdExtractor {
public:
    [[nodiscard]] static Result<std::unique_ptr<DnnReIdExtractor>> Create(DnnReIdOptions options);

    [[nodiscard]] Result<Embedding> Extract(const cv::Mat& bgr, const cv::Rect& roi) const override;

    [[nodiscard]] int Dim() const noexcept override {
        return m_dim;
    }

    [[nodiscard]] std::uint32_t ModelId() const noexcept override {
        return m_options.model_id;
    }

    [[nodiscard]] std::string Describe() const override;

private:
    DnnReIdExtractor(DnnReIdOptions options, cv::dnn::Net net, int dim);

    DnnReIdOptions m_options;
    mutable cv::dnn::Net m_net;
    int m_dim = 0;
};

} // namespace tmcmt
