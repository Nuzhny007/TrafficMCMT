#include "trafficmcmt/edge/TypeAwareReIdExtractor.h"

#include <algorithm>
#include <string>
#include <utility>

namespace tmcmt {
namespace {

Result<std::unique_ptr<DnnReIdExtractor>> CreateDnn(const std::string& path, const TypeAwareReIdOptions& options,
                                                    std::uint32_t modelId) {
    DnnReIdOptions dnn;
    dnn.model_path = path;
    dnn.input_size = options.dnn_input_size;
    dnn.model_id = modelId;
    dnn.mean = options.dnn_mean;
    dnn.stddev = options.dnn_stddev;
    dnn.swap_rb = options.dnn_swap_rb;
    dnn.min_roi_size = options.min_roi_size;
    return DnnReIdExtractor::Create(std::move(dnn));
}

} // namespace

TypeAwareReIdExtractor::TypeAwareReIdExtractor(TypeAwareReIdOptions options,
                                               std::unique_ptr<DnnReIdExtractor> person,
                                               std::unique_ptr<DnnReIdExtractor> vehicle)
    : m_options(std::move(options)),
      m_person(std::move(person)),
      m_vehicle(std::move(vehicle)),
      m_fallbackColor(ColorHistogramOptions{m_options.fallback_bins, m_options.fallback_model_id,
                                            m_options.min_roi_size}),
      m_vehicleColor(
          ColorHistogramOptions{m_options.fallback_bins, m_options.vehicle_model_id, m_options.min_roi_size}) {
    m_personDim = m_person ? m_person->Dim() : m_fallbackColor.Dim();
    m_personModelId = m_person ? m_person->ModelId() : m_options.fallback_model_id;
}

Result<std::unique_ptr<TypeAwareReIdExtractor>> TypeAwareReIdExtractor::Create(TypeAwareReIdOptions options) {
    options.min_roi_size = std::max(1, options.min_roi_size);
    options.fallback_bins = std::clamp(options.fallback_bins, 1, 16);
    if (options.dnn_input_size.width <= 0 || options.dnn_input_size.height <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "type-aware ReID input size must be positive");
    }

    std::unique_ptr<DnnReIdExtractor> person;
    if (!options.person_model_path.empty()) {
        auto created = CreateDnn(options.person_model_path, options, options.person_model_id);
        if (!created) {
            return created.status();
        }
        person = std::move(created).value();
    }

    std::unique_ptr<DnnReIdExtractor> vehicle;
    if (!options.vehicle_model_path.empty()) {
        auto created = CreateDnn(options.vehicle_model_path, options, options.vehicle_model_id);
        if (!created) {
            return created.status();
        }
        vehicle = std::move(created).value();
    }

    std::unique_ptr<TypeAwareReIdExtractor> extractor(
        new TypeAwareReIdExtractor(std::move(options), std::move(person), std::move(vehicle)));
    return extractor;
}

bool TypeAwareReIdExtractor::IsVehicle(ObjectType type) noexcept {
    switch (type) {
    case ObjectType::Car:
    case ObjectType::Truck:
    case ObjectType::Bus:
    case ObjectType::Bike:
        return true;
    case ObjectType::Unknown:
    case ObjectType::Pedestrian:
        return false;
    }
    return false;
}

std::uint32_t TypeAwareReIdExtractor::ModelIdFor(ObjectType type) const noexcept {
    if (type == ObjectType::Pedestrian) {
        return m_personModelId;
    }
    if (IsVehicle(type)) {
        return m_vehicle ? m_vehicle->ModelId() : m_options.vehicle_model_id;
    }
    return m_options.fallback_model_id;
}

int TypeAwareReIdExtractor::DimFor(ObjectType type) const noexcept {
    if (type == ObjectType::Pedestrian) {
        return m_personDim;
    }
    if (IsVehicle(type)) {
        return m_vehicle ? m_vehicle->Dim() : m_vehicleColor.Dim();
    }
    return m_fallbackColor.Dim();
}

Result<Embedding> TypeAwareReIdExtractor::Extract(const cv::Mat& bgr, const cv::Rect& roi) const {
    return m_fallbackColor.Extract(bgr, roi);
}

Result<Embedding> TypeAwareReIdExtractor::Extract(const cv::Mat& bgr, const cv::Rect& roi,
                                                  ObjectType type) const {
    if (type == ObjectType::Pedestrian) {
        return m_person ? m_person->Extract(bgr, roi) : m_fallbackColor.Extract(bgr, roi);
    }
    if (IsVehicle(type)) {
        return m_vehicle ? m_vehicle->Extract(bgr, roi) : m_vehicleColor.Extract(bgr, roi);
    }
    return m_fallbackColor.Extract(bgr, roi);
}

std::string TypeAwareReIdExtractor::Describe() const {
    std::string description = "type-aware(";
    description += "person=" + (m_person ? m_person->Describe() : std::string("hsv")) + ", ";
    description += "vehicle=" + (m_vehicle ? m_vehicle->Describe() : std::string("hsv")) + ", ";
    description += "fallback=" + m_fallbackColor.Describe() + ")";
    return description;
}

} // namespace tmcmt
