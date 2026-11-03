#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/edge/ColorHistogramReIdExtractor.h"
#include "trafficmcmt/edge/DnnReIdExtractor.h"
#include "trafficmcmt/edge/IReIdExtractor.h"

namespace tmcmt {

struct TypeAwareReIdOptions {
    /// Optional ONNX ReID model for pedestrians (e.g. OSNet).
    std::string person_model_path;
    /// Optional ONNX ReID model for vehicles. When empty, the HSV fallback is
    /// used for cars, trucks, buses and bikes.
    std::string vehicle_model_path;

    cv::Size dnn_input_size{128, 256};
    cv::Scalar dnn_mean{0.485, 0.456, 0.406};
    cv::Scalar dnn_stddev{0.229, 0.224, 0.225};
    bool dnn_swap_rb = true;

    std::uint32_t person_model_id = 2;
    std::uint32_t vehicle_model_id = 3;
    std::uint32_t fallback_model_id = 1;

    int fallback_bins = 8;
    int min_roi_size = 8;
};

/// ReID extractor that picks a descriptor model by object type.
///
/// * pedestrians: `person_model_path` (DNN) if configured, otherwise HSV;
/// * vehicles: `vehicle_model_path` (DNN) if configured, otherwise HSV;
/// * unknown: HSV.
///
/// Each branch reports its own `model_id`, so the Master never mixes
/// descriptors produced by different models or by different modalities.
class TypeAwareReIdExtractor final : public IReIdExtractor {
public:
    [[nodiscard]] static Result<std::unique_ptr<TypeAwareReIdExtractor>> Create(TypeAwareReIdOptions options);

    [[nodiscard]] Result<Embedding> Extract(const cv::Mat& bgr, const cv::Rect& roi) const override;

    [[nodiscard]] Result<Embedding> Extract(const cv::Mat& bgr, const cv::Rect& roi,
                                            ObjectType type) const override;

    /// Dimension of the pedestrian descriptor (or of the fallback when no
    /// person model is configured).
    [[nodiscard]] int Dim() const noexcept override {
        return m_personDim;
    }

    /// Model id of the pedestrian descriptor (or of the fallback).
    [[nodiscard]] std::uint32_t ModelId() const noexcept override {
        return m_personModelId;
    }

    [[nodiscard]] std::uint32_t ModelIdFor(ObjectType type) const noexcept;
    [[nodiscard]] int DimFor(ObjectType type) const noexcept;

    [[nodiscard]] bool HasPersonModel() const noexcept {
        return m_person != nullptr;
    }

    [[nodiscard]] bool HasVehicleModel() const noexcept {
        return m_vehicle != nullptr;
    }

    [[nodiscard]] std::string Describe() const override;

private:
    TypeAwareReIdExtractor(TypeAwareReIdOptions options, std::unique_ptr<DnnReIdExtractor> person,
                           std::unique_ptr<DnnReIdExtractor> vehicle);

    static bool IsVehicle(ObjectType type) noexcept;

    TypeAwareReIdOptions m_options;
    std::unique_ptr<DnnReIdExtractor> m_person;
    std::unique_ptr<DnnReIdExtractor> m_vehicle;
    ColorHistogramReIdExtractor m_fallbackColor;
    ColorHistogramReIdExtractor m_vehicleColor;
    int m_personDim = 0;
    std::uint32_t m_personModelId = 0;
};

} // namespace tmcmt
