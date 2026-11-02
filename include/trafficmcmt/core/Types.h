#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tmcmt {

/// Stable camera identifier, e.g. "cam-01-north".
using CameraId = std::string;

/// Unique tracklet identifier: (camera_hash << 32) | local_sequence.
using TrackletId = std::uint64_t;

/// Global identity, shared by tracklets of the same object on all cameras.
using GlobalId = std::uint64_t;

/// Milliseconds since Unix epoch (UTC).
using Timestamp = std::chrono::milliseconds;

inline constexpr GlobalId kInvalidGlobalId = 0;

enum class PipelineMode : std::uint8_t {
    Offline = 0,
    Online = 1,
};

enum class ObjectType : std::uint8_t {
    Unknown = 0,
    Pedestrian = 1,
    Car = 2,
    Truck = 3,
    Bus = 4,
    Bike = 5,
};

inline const char* ToString(PipelineMode mode) noexcept {
    switch (mode) {
    case PipelineMode::Offline:
        return "offline";
    case PipelineMode::Online:
        return "online";
    }
    return "offline";
}

inline const char* ToString(ObjectType type) noexcept {
    switch (type) {
    case ObjectType::Unknown:
        return "unknown";
    case ObjectType::Pedestrian:
        return "pedestrian";
    case ObjectType::Car:
        return "car";
    case ObjectType::Truck:
        return "truck";
    case ObjectType::Bus:
        return "bus";
    case ObjectType::Bike:
        return "bike";
    }
    return "unknown";
}

namespace detail {

inline bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto a = static_cast<unsigned char>(lhs[i]);
        const auto b = static_cast<unsigned char>(rhs[i]);
        const auto lowerA = (a >= 'A' && a <= 'Z') ? static_cast<unsigned char>(a - 'A' + 'a') : a;
        const auto lowerB = (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b - 'A' + 'a') : b;
        if (lowerA != lowerB) {
            return false;
        }
    }
    return true;
}

} // namespace detail

inline std::optional<PipelineMode> ParsePipelineMode(std::string_view text) noexcept {
    if (detail::EqualsIgnoreCase(text, "offline")) {
        return PipelineMode::Offline;
    }
    if (detail::EqualsIgnoreCase(text, "online")) {
        return PipelineMode::Online;
    }
    return std::nullopt;
}

inline std::optional<ObjectType> ParseObjectType(std::string_view text) noexcept {
    if (detail::EqualsIgnoreCase(text, "unknown")) {
        return ObjectType::Unknown;
    }
    if (detail::EqualsIgnoreCase(text, "pedestrian")) {
        return ObjectType::Pedestrian;
    }
    if (detail::EqualsIgnoreCase(text, "car")) {
        return ObjectType::Car;
    }
    if (detail::EqualsIgnoreCase(text, "truck")) {
        return ObjectType::Truck;
    }
    if (detail::EqualsIgnoreCase(text, "bus")) {
        return ObjectType::Bus;
    }
    if (detail::EqualsIgnoreCase(text, "bike")) {
        return ObjectType::Bike;
    }
    return std::nullopt;
}

} // namespace tmcmt
