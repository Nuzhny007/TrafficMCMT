#include "trafficmcmt/emissions/EmissionSimulator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>

#include "trafficmcmt/emissions/GaussianPuffModel.h"
#include "trafficmcmt/emissions/HbefaEmissionModel.h"

namespace tmcmt::emissions {
namespace {

constexpr double kMillisecondsPerSecond = 1000.0;
constexpr double kGramsToMicrograms = 1.0e6;

} // namespace

EnuPoint WindConfig::Vector() const noexcept {
    // Meteorological "from" direction: the advection vector points the
    // opposite way. 0 deg = wind from the north, 90 deg = wind from the east.
    const double radians = from_deg * std::numbers::pi / 180.0;
    return EnuPoint{-speed_mps * std::sin(radians), -speed_mps * std::cos(radians)};
}

EmissionSimulator::EmissionSimulator(EmissionConfig config, GeoPoint origin)
    : m_config(std::move(config)),
      m_origin(origin),
      m_model(std::make_unique<HbefaEmissionModel>(m_config.pm25_fraction,
                                                   m_config.clamp_negative_acceleration)),
      m_grid(MakeSpec()) {}

EmissionSimulator::EmissionSimulator(EmissionConfig config, GeoPoint origin,
                                     std::unique_ptr<IEmissionModel> model)
    : m_config(std::move(config)),
      m_origin(origin),
      m_model(std::move(model)),
      m_grid(MakeSpec()) {}

EmissionSimulator::~EmissionSimulator() = default;
EmissionSimulator::EmissionSimulator(EmissionSimulator&&) noexcept = default;
EmissionSimulator& EmissionSimulator::operator=(EmissionSimulator&&) noexcept = default;

GridSpec EmissionSimulator::MakeSpec() const {
    const EmissionGridConfig& grid = m_config.grid;
    const double cell = std::max(grid.cell_m, 0.1);
    const int max_cols = std::max(grid.max_cols, 1);
    const int max_rows = std::max(grid.max_rows, 1);

    const int cols = std::clamp(static_cast<int>(std::ceil(std::max(grid.width_m, cell) / cell)), 1, max_cols);
    const int rows = std::clamp(static_cast<int>(std::ceil(std::max(grid.height_m, cell) / cell)), 1, max_rows);

    EnuPoint center{0.0, 0.0};
    if (grid.has_center) {
        center = GeographicToEnu(GeoPoint{grid.center_latitude, grid.center_longitude}, m_origin);
    }

    GridSpec spec;
    spec.cell_x_m = cell;
    spec.cell_y_m = cell;
    spec.cols = cols;
    spec.rows = rows;
    spec.south_west.east = center.east - 0.5 * static_cast<double>(cols) * cell;
    spec.south_west.north = center.north - 0.5 * static_cast<double>(rows) * cell;
    return spec;
}

void EmissionSimulator::Step(Timestamp now, const std::vector<EmissionSource>& sources) {
    const EnuPoint wind = m_config.wind.Vector();
    const Timestamp max_age(static_cast<std::int64_t>(std::max(m_config.max_age_s, 0.0) * kMillisecondsPerSecond));

    // Puffs are appended in birth order, so the oldest live at the front.
    while (!m_puffs.empty() && now - m_puffs.front().birth > max_age) {
        m_puffs.pop_front();
    }
    for (PuffState& puff : m_puffs) {
        puff.age_s = static_cast<double>((now - puff.birth).count()) / kMillisecondsPerSecond;
        puff.PlaceAt(wind);
    }

    const Timestamp interval(
        static_cast<std::int64_t>(std::max(m_config.puff_interval_s, 0.1) * kMillisecondsPerSecond));

    ++m_generation;
    for (const EmissionSource& source : sources) {
        EmitterState& emitter = m_emitters[source.track_id];
        emitter.last_seen_generation = m_generation;
        if (!emitter.started) {
            emitter.started = true;
            // Release the first puff immediately with one interval worth of mass.
            emitter.last_spawn = now - interval;
        }
        if (now - emitter.last_spawn < interval) {
            continue;
        }
        const double elapsed = static_cast<double>((now - emitter.last_spawn).count()) / kMillisecondsPerSecond;
        Spawn(now, source, elapsed);
        emitter.last_spawn = now;
    }

    // Drop emitters that were not seen this tick. Comparing the generation
    // rather than the container size also covers a track disappearing while
    // another appears, which would otherwise keep a stale `last_spawn` and
    // release a gap-sized puff when the original id returns.
    for (auto it = m_emitters.begin(); it != m_emitters.end();) {
        if (it->second.last_seen_generation != m_generation) {
            it = m_emitters.erase(it);
        } else {
            ++it;
        }
    }
}

void EmissionSimulator::Spawn(Timestamp now, const EmissionSource& source, double elapsed_s) {
    const EmissionRates rates =
        m_model->Rates(source.class_name, source.speed_mps, source.acceleration_mps2);

    double total = 0.0;
    for (const double rate : rates.grams_per_second) {
        total += rate;
    }
    if (!(total > 0.0)) {
        return;
    }

    PuffState puff;
    puff.id = m_next_puff_id++;
    puff.birth = now;
    puff.origin = source.position;
    puff.center = source.position;
    puff.age_s = 0.0;
    puff.stability = m_config.stability;
    puff.source_height_m = m_config.source_height_m;
    for (std::size_t pollutant = 0; pollutant < kPollutantCount; ++pollutant) {
        puff.mass_g[pollutant] = rates.grams_per_second[pollutant] * elapsed_s;
    }

    m_puffs.push_back(puff);
    const std::size_t limit = std::max<std::size_t>(m_config.max_puffs, 1);
    while (m_puffs.size() > limit) {
        m_puffs.pop_front();
    }
}

bool EmissionSimulator::GridDue(Timestamp now) const noexcept {
    if (!m_has_grid) {
        return true;
    }
    const Timestamp period(
        static_cast<std::int64_t>(kMillisecondsPerSecond / std::max(m_config.grid_hz, 0.01)));
    return now - m_last_grid >= period;
}

std::optional<EmissionGridSnapshot> EmissionSimulator::BuildSnapshot(Timestamp now) {
    if (!m_grid.Spec().IsValid()) {
        return std::nullopt;
    }

    m_grid.Clear();
    const EnuPoint wind = m_config.wind.Vector();
    const double wind_speed = std::hypot(wind.east, wind.north);

    for (const PuffState& puff : m_puffs) {
        const PuffGeometry geometry = ComputePuffGeometry(puff, wind_speed, m_config.mixing_height_m);
        if (!geometry.IsValid()) {
            continue;
        }

        double peak_mass = 0.0;
        for (const double mass : puff.mass_g) {
            peak_mass = std::max(peak_mass, mass);
        }
        // The reflection term doubles the ground-level peak at most.
        const double peak_ug_m3 = peak_mass * geometry.inverse_norm * 2.0 * kGramsToMicrograms;
        if (peak_ug_m3 < m_config.min_peak_ug_m3) {
            continue;
        }

        m_grid.AddPuff(puff, geometry, m_config.breathing_height_m);
    }

    const GridSpec& spec = m_grid.Spec();

    EmissionGridSnapshot snapshot;
    snapshot.timestamp = now;
    snapshot.cols = spec.cols;
    snapshot.rows = spec.rows;
    snapshot.south_west = EnuToGeographic(spec.south_west, m_origin);
    const EnuPoint north_east{spec.south_west.east + static_cast<double>(spec.cols) * spec.cell_x_m,
                              spec.south_west.north + static_cast<double>(spec.rows) * spec.cell_y_m};
    snapshot.north_east = EnuToGeographic(north_east, m_origin);

    for (std::size_t pollutant = 0; pollutant < kPollutantCount; ++pollutant) {
        snapshot.fields[pollutant] = m_grid.Field(static_cast<Pollutant>(pollutant));
    }
    snapshot.stats = m_grid.Stats();

    m_last_grid = now;
    m_has_grid = true;
    return snapshot;
}

void EmissionSimulator::Reset() {
    m_puffs.clear();
    m_emitters.clear();
    m_grid.Clear();
    m_last_grid = Timestamp{};
    m_has_grid = false;
    m_next_puff_id = 1;
}

} // namespace tmcmt::emissions
