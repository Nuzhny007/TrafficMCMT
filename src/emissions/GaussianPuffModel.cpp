#include "trafficmcmt/emissions/GaussianPuffModel.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tmcmt::emissions {

void PuffState::Advect(const EnuPoint& wind_mps, double dt_s) noexcept {
    center.east += wind_mps.east * dt_s;
    center.north += wind_mps.north * dt_s;
}

void PuffState::PlaceAt(const EnuPoint& wind_mps) noexcept {
    center.east = origin.east + wind_mps.east * age_s;
    center.north = origin.north + wind_mps.north * age_s;
}

PuffGeometry ComputePuffGeometry(const PuffState& puff, double wind_speed_mps,
                                 double mixing_height_m) noexcept {
    // x is the downwind distance travelled by the puff, not the distance from
    // the source to the observation point. Units: m/s * s = m.
    const double wind = std::max(wind_speed_mps, kMinDispersionWindMps);
    const double downwind = wind * std::max(puff.age_s, 0.0);

    PuffGeometry geometry;
    geometry.sigma_x = std::max(SigmaX(puff.stability, downwind), kMinSigmaM);
    geometry.sigma_y = std::max(SigmaY(puff.stability, downwind), kMinSigmaM);
    geometry.sigma_z = std::clamp(SigmaZ(puff.stability, downwind), kMinSigmaM,
                                  std::max(mixing_height_m, kMinSigmaM));

    const double norm = std::pow(2.0 * std::numbers::pi, 1.5) * geometry.sigma_x * geometry.sigma_y *
                        geometry.sigma_z;
    geometry.inverse_norm = 1.0 / norm;
    return geometry;
}

double ConcentrationAt(const PuffState& puff, const PuffGeometry& geometry, double mass_g,
                       const EnuPoint& point, double z_m) noexcept {
    if (!geometry.IsValid() || !(mass_g > 0.0)) {
        return 0.0;
    }

    const double dx = point.east - puff.center.east;
    const double dy = point.north - puff.center.north;
    const double two_sx2 = 2.0 * geometry.sigma_x * geometry.sigma_x;
    const double two_sy2 = 2.0 * geometry.sigma_y * geometry.sigma_y;
    const double two_sz2 = 2.0 * geometry.sigma_z * geometry.sigma_z;

    const double dz_up = z_m - puff.source_height_m;
    const double dz_down = z_m + puff.source_height_m;
    const double vertical = std::exp(-(dz_up * dz_up) / two_sz2) + std::exp(-(dz_down * dz_down) / two_sz2);

    return mass_g * geometry.inverse_norm * std::exp(-(dx * dx) / two_sx2) * std::exp(-(dy * dy) / two_sy2) *
           vertical;
}

} // namespace tmcmt::emissions
