#include "trafficmcmt/edge/ColorHistogramReIdExtractor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace tmcmt {

ColorHistogramReIdExtractor::ColorHistogramReIdExtractor(ColorHistogramOptions options) : m_options(options) {
    m_options.bins = std::clamp(m_options.bins, 1, 16);
    m_options.min_roi_size = std::max(1, m_options.min_roi_size);
    m_dim = m_options.bins * m_options.bins * m_options.bins;
}

Result<Embedding> ColorHistogramReIdExtractor::Extract(const cv::Mat& bgr, const cv::Rect& roi) const {
    if (bgr.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "ReID input image is empty");
    }

    cv::Mat image;
    if (bgr.channels() == 1) {
        cv::cvtColor(bgr, image, cv::COLOR_GRAY2BGR);
    } else if (bgr.channels() == 4) {
        cv::cvtColor(bgr, image, cv::COLOR_BGRA2BGR);
    } else if (bgr.channels() == 3) {
        image = bgr;
    } else {
        return Status::Error(ErrorCode::InvalidArgument, "ReID input must have 1, 3 or 4 channels");
    }

    const cv::Rect imageRect(0, 0, image.cols, image.rows);
    const cv::Rect clamped = roi & imageRect;
    if (clamped.width < m_options.min_roi_size || clamped.height < m_options.min_roi_size) {
        return Status::Error(ErrorCode::InvalidArgument, "ReID region is too small or outside the image");
    }

    cv::Mat hsv;
    cv::cvtColor(image(clamped), hsv, cv::COLOR_BGR2HSV);

    const int bins = m_options.bins;
    const int histSize[] = {bins, bins, bins};
    const float hueRange[] = {0.f, 180.f};
    const float satRange[] = {0.f, 256.f};
    const float valRange[] = {0.f, 256.f};
    const float* ranges[] = {hueRange, satRange, valRange};
    const int channels[] = {0, 1, 2};

    cv::Mat histogram;
    cv::calcHist(&hsv, 1, channels, cv::Mat(), histogram, 3, histSize, ranges, true, false);

    const cv::Mat flat = histogram.reshape(1, 1);
    std::vector<float> values(static_cast<std::size_t>(m_dim), 0.f);
    for (int i = 0; i < m_dim; ++i) {
        values[static_cast<std::size_t>(i)] = flat.at<float>(0, i);
    }

    float norm = 0.f;
    for (const float value : values) {
        norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm > 0.f) {
        for (float& value : values) {
            value /= norm;
        }
    }

    Embedding embedding;
    embedding.values = std::move(values);
    embedding.dim = static_cast<std::uint16_t>(m_dim);
    embedding.model_id = m_options.model_id;
    return embedding;
}

std::string ColorHistogramReIdExtractor::Describe() const {
    return "color-hsv-histogram(bins=" + std::to_string(m_options.bins) + ", dim=" + std::to_string(m_dim) + ")";
}

} // namespace tmcmt
