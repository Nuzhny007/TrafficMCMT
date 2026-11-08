#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Types.h"
#include "trafficmcmt/emissions/ConcentrationGrid.h"
#include "trafficmcmt/emissions/EmissionConfig.h"
#include "trafficmcmt/emissions/IEmissionModel.h"

namespace tmcmt::emissions {

/// Immutable concentration map produced at `grid_hz`.
struct EmissionGridSnapshot {
    Timestamp timestamp{};
    GeoPoint south_west{};
    GeoPoint north_east{};
    int cols = 0;
    int rows = 0;
    /// Row-major, south -> north, in ug/m3.
    std::array<std::vector<float>, kPollutantCount> fields;
    std::array<EmissionFieldStats, kPollutantCount> stats;
};

/// Owns the puff population, spawns puffs from tracked vehicles and rasterises
/// the concentration field.
///
/// The simulator is transport-agnostic: `MapFeed` drives it from replay or live
/// tracklets, and unit tests drive it with synthetic sources.
class EmissionSimulator {
public:
    EmissionSimulator(EmissionConfig config, GeoPoint origin);
    EmissionSimulator(EmissionConfig config, GeoPoint origin, std::unique_ptr<IEmissionModel> model);
    ~EmissionSimulator();

    EmissionSimulator(EmissionSimulator&&) noexcept;
    EmissionSimulator& operator=(EmissionSimulator&&) noexcept;
    EmissionSimulator(const EmissionSimulator&) = delete;
    EmissionSimulator& operator=(const EmissionSimulator&) = delete;

    /// Advances puff ages, spawns new puffs and drops expired ones. Call at the
    /// map tick rate; rasterisation happens separately in `BuildSnapshot`.
    void Step(Timestamp now, const std::vector<EmissionSource>& sources);

    /// True when a new concentration frame is due.
    [[nodiscard]] bool GridDue(Timestamp now) const noexcept;

    /// Rasterises the current puff population. Returns nullopt when the grid is
    /// disabled or invalid.
    [[nodiscard]] std::optional<EmissionGridSnapshot> BuildSnapshot(Timestamp now);

    [[nodiscard]] std::size_t PuffCount() const noexcept {
        return m_puffs.size();
    }

    [[nodiscard]] std::size_t EmitterCount() const noexcept {
        return m_emitters.size();
    }

    [[nodiscard]] const EmissionConfig& Config() const noexcept {
        return m_config;
    }

    [[nodiscard]] const GridSpec& Spec() const noexcept {
        return m_grid.Spec();
    }

    [[nodiscard]] const ConcentrationGrid& Grid() const noexcept {
        return m_grid;
    }

    void Reset();

private:
    struct EmitterState {
        Timestamp last_spawn{};
        bool started = false;
        /// Tick generation that last saw this track. Stale entries are dropped
        /// even when the number of tracks stays the same.
        std::uint64_t last_seen_generation = 0;
    };

    [[nodiscard]] GridSpec MakeSpec() const;
    void Spawn(Timestamp now, const EmissionSource& source, double elapsed_s);

    EmissionConfig m_config;
    GeoPoint m_origin{};
    std::unique_ptr<IEmissionModel> m_model;
    ConcentrationGrid m_grid;
    std::deque<PuffState> m_puffs;
    std::unordered_map<std::uint64_t, EmitterState> m_emitters;
    Timestamp m_last_grid{};
    bool m_has_grid = false;
    std::uint64_t m_next_puff_id = 1;
    std::uint64_t m_generation = 0;
};

} // namespace tmcmt::emissions
