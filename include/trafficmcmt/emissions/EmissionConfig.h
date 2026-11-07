#pragma once

#include <cstddef>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/emissions/PasquillGifford.h"

namespace tmcmt::emissions {

/// Mean wind. `from_deg` is the meteorological direction the wind blows from
/// (0 = north, 90 = east), so the advection vector points the opposite way.
struct WindConfig {
    double speed_mps = 3.0;
    double from_deg = 225.0;

    [[nodiscard]] EnuPoint Vector() const noexcept;
};

/// Explicit raster placement. When `has_center` is false the grid is centred on
/// the map origin, which keeps the viewer bounds stable across frames.
struct EmissionGridConfig {
    bool has_center = false;
    double center_latitude = 0.0;
    double center_longitude = 0.0;
    double width_m = 600.0;
    double height_m = 450.0;
    double cell_m = 5.0;
    int max_cols = 256;
    int max_rows = 256;
};

struct EmissionConfig {
    bool enabled = false;
    StabilityClass stability = StabilityClass::D;
    WindConfig wind{};
    double source_height_m = 0.5;
    double breathing_height_m = 1.5;
    double mixing_height_m = 1000.0;
    double puff_interval_s = 1.0;
    double max_age_s = 180.0;
    std::size_t max_puffs = 3000;
    double pm25_fraction = 0.5;
    double grid_hz = 1.0;
    /// Puffs whose peak contribution is below this are skipped (ug/m3).
    double min_peak_ug_m3 = 1e-4;
    bool clamp_negative_acceleration = true;
    EmissionGridConfig grid{};
};

} // namespace tmcmt::emissions
