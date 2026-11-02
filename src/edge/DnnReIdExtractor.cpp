#include "trafficmcmt/edge/DnnReIdExtractor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace tmcmt {

DnnReIdExtractor::DnnReIdExtractor(DnnReIdOptions options, cv::dnn::Net net, int dim)
    : m_options(std::move(options)), m_net(std::move(net)), m_dim(dim) {}

Result<std::unique_ptr<DnnReIdExtractor>> DnnReIdExtractor::Create(DnnReIdOptions options) {
    if (options.model_path.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "DNN ReID model path must not be empty");
    }
    if (!std::filesystem::exists(options.model_path)) {
        return Status::Error(ErrorCode::NotFound, "DNN ReID model not found: " + options.model_path);
    }
    if (options.input_size.width <= 0 || options.input_size.height <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "DNN ReID input size must be positive");
    }

    try {
        cv::dnn::Net net = cv::dnn::readNetFromONNX(options.model_path);
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

        // Probe the output dimension with a black image.
        const cv::Mat probe(options.input_size, CV_8UC3, cv::Scalar::all(0));
        const cv::Mat blob =
            cv::dnn::blobFromImage(probe, options.scale, options.input_size, options.mean, options.swap_rb, false);
        net.setInput(blob);
        const cv::Mat output = net.forward();
        const int dim = static_cast<int>(output.total());
        if (dim <= 0 || dim > 65535) {
            return Status::Error(ErrorCode::InvalidArgument, "DNN ReID output dimension is not supported");
        }

        std::unique_ptr<DnnReIdExtractor> extractor(new DnnReIdExtractor(std::move(options), std::move(net), dim));
        return extractor;
    } catch (const cv::Exception& e) {
        return Status::Error(ErrorCode::Internal, std::string("cannot load DNN ReID model: ") + e.what());
    }
}

Result<Embedding> DnnReIdExtractor::Extract(const cv::Mat& bgr, const cv::Rect& roi) const {
    if (bgr.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "ReID input image is empty");
    }

    const cv::Rect clamped = roi & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (clamped.width < m_options.min_roi_size || clamped.height < m_options.min_roi_size) {
        return Status::Error(ErrorCode::InvalidArgument, "ReID region is too small or outside the image");
    }

    try {
        cv::Mat crop = bgr(clamped);
        cv::Mat resized;
        cv::resize(crop, resized, m_options.input_size);

        const cv::Mat blob =
            cv::dnn::blobFromImage(resized, m_options.scale, m_options.input_size, m_options.mean, m_options.swap_rb,
                                   false);
        m_net.setInput(blob);
        const cv::Mat output = m_net.forward();
        const cv::Mat flat = output.reshape(1, 1);

        std::vector<float> values(static_cast<std::size_t>(m_dim), 0.f);
        for (int i = 0; i < m_dim; ++i) {
            values[static_cast<std::size_t>(i)] = flat.at<float>(0, i);
        }

        float norm = 0.f;
        for (const float value : values) {
            if (!std::isfinite(value)) {
                return Status::Error(ErrorCode::Internal, "DNN ReID produced a non-finite value");
            }
            norm += value * value;
        }
        norm = std::sqrt(norm);
        if (!(norm > 0.f)) {
            return Status::Error(ErrorCode::Internal, "DNN ReID produced a zero embedding");
        }
        for (float& value : values) {
            value /= norm;
        }

        Embedding embedding;
        embedding.values = std::move(values);
        embedding.dim = static_cast<std::uint16_t>(m_dim);
        embedding.model_id = m_options.model_id;
        return embedding;
    } catch (const cv::Exception& e) {
        return Status::Error(ErrorCode::Internal, std::string("DNN ReID inference failed: ") + e.what());
    }
}

std::string DnnReIdExtractor::Describe() const {
    return "dnn(" + m_options.model_path + ", dim=" + std::to_string(m_dim) + ")";
}

} // namespace tmcmt
