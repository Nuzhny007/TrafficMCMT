#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace tmcmt::emissions {

/// Pasquill-Gifford stability classes, from very unstable (A) to very stable
/// (F). `D` is neutral and is the default for urban traffic.
enum class StabilityClass : std::uint8_t {
    A = 0,
    B = 1,
    C = 2,
    D = 3,
    E = 4,
    F = 5,
};

[[nodiscard]] const char* StabilityClassName(StabilityClass stability) noexcept;
[[nodiscard]] std::optional<StabilityClass> ParseStabilityClass(std::string_view text) noexcept;

/// Horizontal dispersion coefficient in metres.
///
/// `downwind_m` is the distance the puff has travelled from the source:
/// `downwind_m = wind_speed [m/s] * age [s]`. The published coefficients are
/// calibrated for x in metres, so `x` is used directly (no /1000 rescaling).
/// Reference values: sigma_y(D, 1 km) = 75 m, sigma_y(A, 1 km) = 187 m.
[[nodiscard]] double SigmaY(StabilityClass stability, double downwind_m) noexcept;

/// Vertical dispersion coefficient in metres.
///
/// Same distance convention as `SigmaY`. Reference values:
/// sigma_z(D, 1 km) = 29 m, sigma_z(A, 1 km) = 347 m.
[[nodiscard]] double SigmaZ(StabilityClass stability, double downwind_m) noexcept;

/// Puff models have no independent horizontal axis; the standard assumption is
/// `sigma_x = sigma_y`.
[[nodiscard]] inline double SigmaX(StabilityClass stability, double downwind_m) noexcept {
    return SigmaY(stability, downwind_m);
}

} // namespace tmcmt::emissions
