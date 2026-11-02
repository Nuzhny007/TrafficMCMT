#include "trafficmcmt/mtt/MttTrackAdapter.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "mtracking/object_types.h"
#include "mtracking/trajectory.h"

namespace tmcmt {
namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

MttTrackAdapter::MttTrackAdapter(MttAdapterOptions options) : m_options(std::move(options)) {}

bool MttTrackAdapter::ShouldExport(const TrackingObject& track) const {
    if (m_options.skip_static && track.m_isStatic) {
        return false;
    }
    if (m_options.skip_out_of_frame && track.m_outOfTheFrame) {
        return false;
    }
    if (m_options.min_trace_length > 0 && track.m_trace.size() < static_cast<std::size_t>(m_options.min_trace_length)) {
        return false;
    }
    if (track.m_confidence >= 0.f && track.m_confidence < m_options.min_confidence) {
        return false;
    }
    return true;
}

BBox MttTrackAdapter::ToBBox(const TrackingObject& track) noexcept {
    const cv::Rect rect = track.m_rrect.boundingRect();
    BBox bbox;
    bbox.x = static_cast<float>(rect.x);
    bbox.y = static_cast<float>(rect.y);
    bbox.w = static_cast<float>(rect.width);
    bbox.h = static_cast<float>(rect.height);
    bbox.score = track.m_confidence;
    return bbox;
}

ObjectType MttTrackAdapter::ToObjectType(int mtt_type) noexcept {
    if (mtt_type == bad_type) {
        return ObjectType::Unknown;
    }
    const std::string name = Lower(TypeConverter::Type2Str(mtt_type));
    if (name == "person" || name == "pedestrian" || name == "people") {
        return ObjectType::Pedestrian;
    }
    if (name == "car" || name == "suv" || name == "pickup" || name == "van") {
        return ObjectType::Car;
    }
    if (name == "truck" || name == "lorry") {
        return ObjectType::Truck;
    }
    if (name == "bus") {
        return ObjectType::Bus;
    }
    if (name == "bicycle" || name == "bike" || name == "motorcycle" || name == "motorbike") {
        return ObjectType::Bike;
    }
    return ObjectType::Unknown;
}

Result<Observation> MttTrackAdapter::ToObservation(const TrackingObject& track, Timestamp ts) const {
    if (!ShouldExport(track)) {
        return Status::Error(ErrorCode::InvalidArgument, "track is filtered out by the adapter options");
    }

    Observation observation;
    observation.ts = ts;
    observation.bbox = ToBBox(track);
    if (!observation.bbox.IsFinite()) {
        return Status::Error(ErrorCode::InvalidArgument, "MTT produced a non-finite bounding box");
    }
    return observation;
}

} // namespace tmcmt
