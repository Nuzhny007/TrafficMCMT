#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/emissions/EmissionTypes.h"
#include "trafficmcmt/emissions/GaussianPuffModel.h"

namespace tmcmt::emissions {

/// Axis-aligned ENU raster. Cell (0, 0) is the south-west corner and rows grow
/// northwards, matching the row-major order of the protocol payload.
struct GridSpec {
    EnuPoint south_west{};
    double cell_x_m = 5.0;
    double cell_y_m = 5.0;
    int cols = 0;
    int rows = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return cols > 0 && rows > 0 && cell_x_m > 0.0 && cell_y_m > 0.0;
    }

    [[nodiscard]] double CellCenterEast(int col) const noexcept {
        return south_west.east + (static_cast<double>(col) + 0.5) * cell_x_m;
    }

    [[nodiscard]] double CellCenterNorth(int row) const noexcept {
        return south_west.north + (static_cast<double>(row) + 0.5) * cell_y_m;
    }

    [[nodiscard]] int Column(double east) const noexcept;
    [[nodiscard]] int Row(double north) const noexcept;

    [[nodiscard]] std::size_t CellCount() const noexcept {
        return static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
    }
};

/// Accumulates puff contributions on a 2D raster at breathing height.
class ConcentrationGrid {
public:
    explicit ConcentrationGrid(GridSpec spec);

    void Clear();

    /// Adds one puff. Cells farther than 2.5 sigma from the puff centre are
    /// skipped, which keeps the cost proportional to the puff footprint.
    ///
    /// Puffs narrower than half a cell are widened to the cell footprint so
    /// that a fresh puff cannot dump its whole mass into a single cell and
    /// produce release-point-dependent spikes.
    void AddPuff(const PuffState& puff, const PuffGeometry& geometry, double z_m);

    [[nodiscard]] const GridSpec& Spec() const noexcept {
        return m_spec;
    }

    [[nodiscard]] const std::vector<float>& Field(Pollutant pollutant) const noexcept {
        return m_fields[static_cast<std::size_t>(pollutant)];
    }

    [[nodiscard]] std::array<EmissionFieldStats, kPollutantCount> Stats() const;

private:
    GridSpec m_spec;
    std::array<std::vector<float>, kPollutantCount> m_fields;
};

} // namespace tmcmt::emissions
