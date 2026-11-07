#include "trafficmcmt/emissions/ConcentrationGrid.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace tmcmt::emissions {
namespace {

/// Puffs contribute to cells within this many sigmas of their centre.
constexpr double kFootprintSigmas = 2.5;

/// Peak concentration conversion from g/m3 to ug/m3.
constexpr double kGramsToMicrograms = 1.0e6;

/// A puff narrower than half a cell would put its whole mass into one cell and
/// produce spikes that depend on where the release point happens to fall. The
/// clamp spreads such a puff over the cell footprint instead.
PuffGeometry FitToCell(PuffGeometry geometry, const GridSpec& spec) noexcept {
    const double min_x = 0.5 * spec.cell_x_m;
    const double min_y = 0.5 * spec.cell_y_m;
    const double min_z = 0.5 * std::max(spec.cell_x_m, spec.cell_y_m);

    const double sigma_x = std::max(geometry.sigma_x, min_x);
    const double sigma_y = std::max(geometry.sigma_y, min_y);
    const double sigma_z = std::max(geometry.sigma_z, min_z);

    if (sigma_x == geometry.sigma_x && sigma_y == geometry.sigma_y && sigma_z == geometry.sigma_z) {
        return geometry;
    }

    geometry.sigma_x = sigma_x;
    geometry.sigma_y = sigma_y;
    geometry.sigma_z = sigma_z;
    const double norm = std::pow(2.0 * std::numbers::pi, 1.5) * sigma_x * sigma_y * sigma_z;
    geometry.inverse_norm = 1.0 / norm;
    return geometry;
}

} // namespace

int GridSpec::Column(double east) const noexcept {
    if (!(cell_x_m > 0.0) || cols <= 0) {
        return -1;
    }
    const double index = std::floor((east - south_west.east) / cell_x_m);
    if (index < 0.0) {
        return -1;
    }
    if (index >= static_cast<double>(cols)) {
        return cols;
    }
    return static_cast<int>(index);
}

int GridSpec::Row(double north) const noexcept {
    if (!(cell_y_m > 0.0) || rows <= 0) {
        return -1;
    }
    const double index = std::floor((north - south_west.north) / cell_y_m);
    if (index < 0.0) {
        return -1;
    }
    if (index >= static_cast<double>(rows)) {
        return rows;
    }
    return static_cast<int>(index);
}

ConcentrationGrid::ConcentrationGrid(GridSpec spec) : m_spec(spec) {
    const std::size_t cells = m_spec.IsValid() ? m_spec.CellCount() : 0;
    for (std::vector<float>& field : m_fields) {
        field.assign(cells, 0.f);
    }
}

void ConcentrationGrid::Clear() {
    for (std::vector<float>& field : m_fields) {
        std::fill(field.begin(), field.end(), 0.f);
    }
}

void ConcentrationGrid::AddPuff(const PuffState& puff, const PuffGeometry& geometry, double z_m) {
    if (!m_spec.IsValid() || !geometry.IsValid()) {
        return;
    }

    const PuffGeometry fitted = FitToCell(geometry, m_spec);

    const double radius_x = kFootprintSigmas * fitted.sigma_x;
    const double radius_y = kFootprintSigmas * fitted.sigma_y;

    int first_col = m_spec.Column(puff.center.east - radius_x);
    int last_col = m_spec.Column(puff.center.east + radius_x);
    int first_row = m_spec.Row(puff.center.north - radius_y);
    int last_row = m_spec.Row(puff.center.north + radius_y);

    if (last_col < 0 || last_row < 0 || first_col >= m_spec.cols || first_row >= m_spec.rows) {
        return;
    }

    first_col = std::max(first_col, 0);
    last_col = std::min(last_col, m_spec.cols - 1);
    first_row = std::max(first_row, 0);
    last_row = std::min(last_row, m_spec.rows - 1);

    const double dz_up = z_m - puff.source_height_m;
    const double dz_down = z_m + puff.source_height_m;
    const double two_sz2 = 2.0 * fitted.sigma_z * fitted.sigma_z;
    const double vertical = std::exp(-(dz_up * dz_up) / two_sz2) + std::exp(-(dz_down * dz_down) / two_sz2);
    if (!(vertical > 0.0)) {
        return;
    }

    const double scale = fitted.inverse_norm * vertical * kGramsToMicrograms;
    const double inverse_two_sx2 = 1.0 / (2.0 * fitted.sigma_x * fitted.sigma_x);
    const double inverse_two_sy2 = 1.0 / (2.0 * fitted.sigma_y * fitted.sigma_y);

    for (int row = first_row; row <= last_row; ++row) {
        const double dy = m_spec.CellCenterNorth(row) - puff.center.north;
        const double weight_y = std::exp(-(dy * dy) * inverse_two_sy2);
        if (weight_y <= 0.0) {
            continue;
        }

        const std::size_t row_offset = static_cast<std::size_t>(row) * static_cast<std::size_t>(m_spec.cols);
        for (int col = first_col; col <= last_col; ++col) {
            const double dx = m_spec.CellCenterEast(col) - puff.center.east;
            const double weight = scale * weight_y * std::exp(-(dx * dx) * inverse_two_sx2);
            if (!(weight > 0.0)) {
                continue;
            }

            const std::size_t index = row_offset + static_cast<std::size_t>(col);
            for (std::size_t pollutant = 0; pollutant < kPollutantCount; ++pollutant) {
                m_fields[pollutant][index] += static_cast<float>(puff.mass_g[pollutant] * weight);
            }
        }
    }
}

std::array<EmissionFieldStats, kPollutantCount> ConcentrationGrid::Stats() const {
    std::array<EmissionFieldStats, kPollutantCount> stats{};

    for (std::size_t pollutant = 0; pollutant < kPollutantCount; ++pollutant) {
        const std::vector<float>& field = m_fields[pollutant];
        if (field.empty()) {
            continue;
        }

        double sum = 0.0;
        float maximum = 0.f;
        for (const float value : field) {
            sum += value;
            maximum = std::max(maximum, value);
        }

        // p95 over the cells that actually carry the plume. Including the
        // empty background would report zero for any reasonably sized grid.
        std::vector<float> sorted;
        sorted.reserve(field.size());
        for (const float value : field) {
            if (value > 0.f) {
                sorted.push_back(value);
            }
        }
        float p95 = 0.f;
        if (!sorted.empty()) {
            const std::size_t percentile =
                static_cast<std::size_t>(0.95 * static_cast<double>(sorted.size() - 1));
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(percentile),
                             sorted.end());
            p95 = sorted[percentile];
        }

        stats[pollutant].max = maximum;
        stats[pollutant].mean = static_cast<float>(sum / static_cast<double>(field.size()));
        stats[pollutant].p95 = p95;
    }

    return stats;
}

} // namespace tmcmt::emissions
