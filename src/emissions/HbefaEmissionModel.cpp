#include "trafficmcmt/emissions/HbefaEmissionModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>

namespace tmcmt::emissions {
namespace {

// HBEFA coefficient blocks, 36 values per vehicle class:
// CO2[0..5], CO[6..11], HC[12..17], FUEL[18..23], NOx[24..29], PMx[30..35].
// Only the three classes reachable from the tracker are carried over.
//
// Provenance: SUMO `HelpersHBEFA.cpp` (DLR, GPL-2.0+). See
// `docs/emissions-attribution.md`.

// SUMO `SVE_HDV_6_6` (heavy duty, 6 clusters, class 6/6).
constexpr std::array<double, 36> kHdv66 = {
    6523.294,    252.2728,    3.844317,     795.2882,   -4.959638,   0.02617982,  65.1358,
    0.3528321,   0.006897636, -0.01911436,  0.02942282, -0.000262364, 30.13205,   0.02454291,
    0.002579182, -0.04914736, 0.007503273,  -7.30E-05,  2054.581,    79.45602,    1.210808,
    250.4845,    -1.56209,    0.008245727,  59.21904,   3.089958,    0.003992818, 10.10062,
    -0.08431282, 0.000517455, 11.9726,      0.05959373, 0.002890182, -0.04753164, 0.005511,
    -4.79E-05,
};

// SUMO `SVE_P_LDV_7_1` (passenger / light delivery, 7 clusters, class 1/7).
constexpr std::array<double, 36> kLdv71 = {
    996.9682,   20.5008,    2.105255,  402.9621,   -6.851369,  0.058854,  103.3263,
    6.420582,   -0.069665,  26.63126,  -1.679889,  0.020307,   28.34217,  0.11478,
    0.003321,   2.221844,   -0.035114, 0.000203,   314.0057,   6.456945,  0.663072,
    126.9172,   -2.157912,  0.018537,  -17.87872,  0.225347,   0.032447,  2.951223,
    -0.008134,  0.000136,   0.0,       0.0,        0.0,        0.0,       0.0,
    0.0,
};

// SUMO `SVE_P_LDV_7_7` (passenger / light delivery, 7 clusters, class 7/7).
constexpr std::array<double, 36> kLdv77 = {
    2166.094,   19.70297,   1.127945,  124.9567,   -0.9551173, 0.013322,   22.93762,
    0.3453138,  0.0340968,  -1.141499, -0.0123665, 0.00048415, 1.257912,   0.0073757,
    0.000494,   -0.0051317, -0.00024665, 6.55E-06, 682.2344,   6.205661,   0.3552582,
    39.35645,   -0.3008244, 0.00419595, 3.022236,  0.0405486,  0.0033863,  0.2024628,
    -0.00173205, 3.28E-05,  0.07654135, 0.00089725, 0.00037255, 0.0085828, -7.94E-05,
    1.50E-06,
};

constexpr std::size_t kCo2Offset = 0;
constexpr std::size_t kCoOffset = 6;
constexpr std::size_t kNoXOffset = 24;
constexpr std::size_t kPmxOffset = 30;

enum class HbefaClass {
    Zero,
    Hdv66,
    Ldv71,
    Ldv77,
};

std::string Lower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

HbefaClass Classify(std::string_view class_name) {
    const std::string name = Lower(class_name);
    if (name == "truck" || name == "bus") {
        return HbefaClass::Hdv66;
    }
    if (name == "bike" || name == "bicycle" || name == "person" || name == "pedestrian") {
        return HbefaClass::Zero;
    }
    if (name == "motorbike" || name == "motorcycle") {
        return HbefaClass::Ldv71;
    }
    // car, vehicle, unknown and anything unexpected fall back to a passenger car.
    return HbefaClass::Ldv77;
}

const std::array<double, 36>& RowFor(HbefaClass hbefa) noexcept {
    switch (hbefa) {
    case HbefaClass::Hdv66:
        return kHdv66;
    case HbefaClass::Ldv71:
        return kLdv71;
    case HbefaClass::Ldv77:
    case HbefaClass::Zero:
        break;
    }
    return kLdv77;
}

std::array<double, 6> Slice(const std::array<double, 36>& row, std::size_t offset) noexcept {
    std::array<double, 6> block{};
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = row[offset + i];
    }
    return block;
}

} // namespace

HbefaEmissionModel::HbefaEmissionModel(double pm25_fraction, bool clamp_negative_acceleration) noexcept
    : m_pm25_fraction(std::clamp(pm25_fraction, 0.0, 1.0)),
      m_clamp_negative_acceleration(clamp_negative_acceleration) {}

double HbefaEmissionModel::ComputeUsing(const std::array<double, 6>& params, double speed_mps,
                                        double acceleration_mps2) noexcept {
    // Faithful port of SUMO HelpersHBEFA::computeUsing.
    if (acceleration_mps2 < 0.0) {
        return 0.0;
    }
    const double v = std::max(speed_mps, 0.0) * 3.6; // km/h
    const double ratio = std::clamp(acceleration_mps2 / 9.81, -1.0, 1.0);
    const double alpha = std::asin(ratio) * 180.0 / std::numbers::pi;

    const double value = (params[0] + params[1] * alpha * v + params[2] * alpha * alpha * v +
                          params[3] * v + params[4] * v * v + params[5] * v * v * v) /
                         3600.0;
    return std::max(value, 0.0);
}

EmissionRates HbefaEmissionModel::Rates(std::string_view class_name, double speed_mps,
                                        double acceleration_mps2) const {
    EmissionRates rates;
    const HbefaClass hbefa = Classify(class_name);
    if (hbefa == HbefaClass::Zero) {
        return rates;
    }

    double acceleration = acceleration_mps2;
    if (acceleration < 0.0) {
        if (!m_clamp_negative_acceleration) {
            return rates;
        }
        acceleration = 0.0;
    }

    const std::array<double, 36>& row = RowFor(hbefa);
    const double speed = std::clamp(speed_mps, 0.0, kMaxHbefaSpeedMps);
    rates[Pollutant::CO2] = ComputeUsing(Slice(row, kCo2Offset), speed, acceleration);
    rates[Pollutant::CO] = ComputeUsing(Slice(row, kCoOffset), speed, acceleration);
    rates[Pollutant::NOx] = ComputeUsing(Slice(row, kNoXOffset), speed, acceleration);

    const double pmx = ComputeUsing(Slice(row, kPmxOffset), speed, acceleration);
    rates[Pollutant::PM10] = pmx;
    rates[Pollutant::PM25] = pmx * m_pm25_fraction;
    return rates;
}

} // namespace tmcmt::emissions
