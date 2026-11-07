#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "trafficmcmt/core/Geo.h"

namespace tmcmt::emissions {

/// Pollutants produced by the HBEFA emission model. `PM25` is derived from the
/// HBEFA `PMx` rate with a configurable fraction; `PM10` is `PMx` itself.
enum class Pollutant : std::uint8_t {
    NOx = 0,
    CO = 1,
    PM25 = 2,
    PM10 = 3,
    CO2 = 4,
};

inline constexpr std::size_t kPollutantCount = 5;

[[nodiscard]] inline const char* PollutantName(Pollutant pollutant) noexcept {
    switch (pollutant) {
    case Pollutant::NOx:
        return "NOx";
    case Pollutant::CO:
        return "CO";
    case Pollutant::PM25:
        return "PM2.5";
    case Pollutant::PM10:
        return "PM10";
    case Pollutant::CO2:
        return "CO2";
    }
    return "unknown";
}

/// Unit of every concentration field sent to the viewer.
[[nodiscard]] inline const char* PollutantUnit() noexcept {
    return "ug/m3";
}

/// Instantaneous emission rates in grams per second.
struct EmissionRates {
    std::array<double, kPollutantCount> grams_per_second{};

    [[nodiscard]] double operator[](Pollutant pollutant) const noexcept {
        return grams_per_second[static_cast<std::size_t>(pollutant)];
    }

    double& operator[](Pollutant pollutant) noexcept {
        return grams_per_second[static_cast<std::size_t>(pollutant)];
    }
};

/// One moving emitter at a single simulation tick.
struct EmissionSource {
    std::uint64_t track_id = 0;
    EnuPoint position{};
    double speed_mps = 0.0;
    double acceleration_mps2 = 0.0;
    std::string_view class_name = "unknown";
};

/// Summary statistics of one concentration field (ug/m3).
struct EmissionFieldStats {
    float max = 0.f;
    float mean = 0.f;
    float p95 = 0.f;
};

} // namespace tmcmt::emissions
