#pragma once

#include <string_view>

#include "trafficmcmt/emissions/EmissionTypes.h"

namespace tmcmt::emissions {

/// Maps a tracked object and its kinematics to instantaneous emission rates.
///
/// The interface keeps the dispersion model independent of the emission
/// inventory: tests can inject a constant-rate model, and a future
/// COPERT/EMEP backend can replace HBEFA without touching the puff code.
class IEmissionModel {
public:
    virtual ~IEmissionModel() = default;

    [[nodiscard]] virtual EmissionRates Rates(std::string_view class_name, double speed_mps,
                                              double acceleration_mps2) const = 0;
};

} // namespace tmcmt::emissions
