#include "trafficmcmt/emissions/PasquillGifford.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <string>

namespace tmcmt::emissions {
namespace {

struct Coefficient {
    double a = 0.0;
    double b = 0.0;
};

// sigma_y = a * x^b, x in metres, sigma_y in metres.
// Reference: sigma_y(D, 1 km) = 75 m, sigma_y(A, 1 km) = 187 m.
constexpr Coefficient kSigmaY[6] = {
    {0.3658, 0.9031}, // A
    {0.2751, 0.9031}, // B
    {0.2090, 0.9031}, // C
    {0.1471, 0.9031}, // D
    {0.1046, 0.9031}, // E
    {0.0722, 0.9031}, // F
};

// sigma_z = c * x^d, x in metres, sigma_z in metres.
// Reference: sigma_z(D, 1 km) = 29 m, sigma_z(A, 1 km) = 347 m.
constexpr Coefficient kSigmaZ[6] = {
    {0.192, 1.0857}, // A
    {0.156, 0.9610}, // B
    {0.116, 0.9037}, // C
    {0.079, 0.8555}, // D
    {0.063, 0.8051}, // E
    {0.053, 0.7573}, // F
};

std::size_t Index(StabilityClass stability) noexcept {
    return static_cast<std::size_t>(stability);
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

const char* StabilityClassName(StabilityClass stability) noexcept {
    switch (stability) {
    case StabilityClass::A:
        return "A";
    case StabilityClass::B:
        return "B";
    case StabilityClass::C:
        return "C";
    case StabilityClass::D:
        return "D";
    case StabilityClass::E:
        return "E";
    case StabilityClass::F:
        return "F";
    }
    return "D";
}

std::optional<StabilityClass> ParseStabilityClass(std::string_view text) noexcept {
    const std::string lower = Lower(std::string(text));
    if (lower == "a") {
        return StabilityClass::A;
    }
    if (lower == "b") {
        return StabilityClass::B;
    }
    if (lower == "c") {
        return StabilityClass::C;
    }
    if (lower == "d") {
        return StabilityClass::D;
    }
    if (lower == "e") {
        return StabilityClass::E;
    }
    if (lower == "f") {
        return StabilityClass::F;
    }
    return std::nullopt;
}

double SigmaY(StabilityClass stability, double downwind_m) noexcept {
    const Coefficient& coefficient = kSigmaY[Index(stability)];
    const double x = std::max(downwind_m, 0.0);
    return coefficient.a * std::pow(x, coefficient.b);
}

double SigmaZ(StabilityClass stability, double downwind_m) noexcept {
    const Coefficient& coefficient = kSigmaZ[Index(stability)];
    const double x = std::max(downwind_m, 0.0);
    return coefficient.a * std::pow(x, coefficient.b);
}

} // namespace tmcmt::emissions
