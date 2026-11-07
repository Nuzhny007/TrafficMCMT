#pragma once

#include <array>
#include <cstdint>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Types.h"
#include "trafficmcmt/emissions/EmissionTypes.h"
#include "trafficmcmt/emissions/PasquillGifford.h"

namespace tmcmt::emissions {

/// Smallest sigma used to keep the Gaussian well defined right after release.
inline constexpr double kMinSigmaM = 0.5;

/// Below this wind speed the puff would never grow; the dispersion distance is
/// advanced with a small floor instead of freezing the plume.
inline constexpr double kMinDispersionWindMps = 0.3;

/// One Lagrangian puff: a fixed parcel of mass released at `origin`.
///
/// The puff is advected by the mean wind and spreads according to the
/// Pasquill-Gifford coefficients evaluated at the downwind distance
/// `x = wind_speed * age`.
struct PuffState {
    std::uint64_t id = 0;
    Timestamp birth{};
    EnuPoint origin{};
    EnuPoint center{};
    double age_s = 0.0;
    std::array<double, kPollutantCount> mass_g{};
    StabilityClass stability = StabilityClass::D;
    double source_height_m = 0.5;

    /// Moves the puff with the mean wind. Kept explicit so a future
    /// time-varying wind field can replace the analytic advection.
    void Advect(const EnuPoint& wind_mps, double dt_s) noexcept;

    /// Repositions the puff from its birth point, `center = origin + wind*age`.
    /// Equivalent to repeated `Advect` calls under a constant wind and immune to
    /// accumulated floating-point drift.
    void PlaceAt(const EnuPoint& wind_mps) noexcept;
};

/// Pre-computed shape of a puff, shared by all pollutants.
struct PuffGeometry {
    double sigma_x = kMinSigmaM;
    double sigma_y = kMinSigmaM;
    double sigma_z = kMinSigmaM;
    /// 1 / ((2*pi)^1.5 * sigma_x * sigma_y * sigma_z)
    double inverse_norm = 0.0;

    [[nodiscard]] bool IsValid() const noexcept {
        return sigma_x > 0.0 && sigma_y > 0.0 && sigma_z > 0.0 && inverse_norm > 0.0;
    }
};

/// Evaluates the dispersion coefficients for the puff's current age.
///
/// `x = max(wind_speed, kMinDispersionWindMps) * age`, so the coefficients are
/// driven by the distance the puff has travelled, not by the distance from the
/// source to the observation point (that convention belongs to steady-state
/// Gaussian plume models).
[[nodiscard]] PuffGeometry ComputePuffGeometry(const PuffState& puff, double wind_speed_mps,
                                               double mixing_height_m = 1000.0) noexcept;

/// Concentration in g/m3 of a puff with the given mass at `point` (ENU, z in
/// metres). Includes the ground-reflection term.
[[nodiscard]] double ConcentrationAt(const PuffState& puff, const PuffGeometry& geometry, double mass_g,
                                     const EnuPoint& point, double z_m) noexcept;

} // namespace tmcmt::emissions
