#pragma once

#include <array>
#include <string_view>

#include "trafficmcmt/emissions/EmissionTypes.h"
#include "trafficmcmt/emissions/IEmissionModel.h"

namespace tmcmt::emissions {

/// HBEFA coefficients are fitted for road-vehicle speeds. Above this limit the
/// cubic term extrapolates to physically meaningless rates (a truck at
/// 250 km/h would emit tens of grams of NOx per second), so `Rates` clamps its
/// input to the table range. `ComputeUsing` stays a byte-for-byte port.
inline constexpr double kMaxHbefaSpeedMps = 130.0 / 3.6;

/// HBEFA emission model ported from the SUMO `HelpersHBEFA` reference module
/// (`C:\work\home\mtracker\TrafficMCMT\emissions`).
///
/// Only the three vehicle classes used by the tracker are carried over:
/// `HDV_6_6`, `P_LDV_7_1` and `P_LDV_7_7`. See `docs/emissions-attribution.md`
/// for provenance and licensing notes.
///
/// The reference formula returns 0 for any negative acceleration. That leaves
/// visible gaps in the concentration map while a vehicle brakes, so by default
/// negative acceleration is clamped to 0 (idle emissions). Set
/// `clamp_negative_acceleration = false` for byte-for-byte reference parity.
class HbefaEmissionModel final : public IEmissionModel {
public:
    explicit HbefaEmissionModel(double pm25_fraction = 0.5,
                                bool clamp_negative_acceleration = true) noexcept;

    [[nodiscard]] EmissionRates Rates(std::string_view class_name, double speed_mps,
                                      double acceleration_mps2) const override;

    /// Direct port of `HelpersHBEFA::computeUsing`: grams per second.
    [[nodiscard]] static double ComputeUsing(const std::array<double, 6>& params, double speed_mps,
                                             double acceleration_mps2) noexcept;

    [[nodiscard]] double Pm25Fraction() const noexcept {
        return m_pm25_fraction;
    }

private:
    double m_pm25_fraction = 0.5;
    bool m_clamp_negative_acceleration = true;
};

} // namespace tmcmt::emissions
