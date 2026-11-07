#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Types.h"
#include "trafficmcmt/emissions/ConcentrationGrid.h"
#include "trafficmcmt/emissions/EmissionConfig.h"
#include "trafficmcmt/emissions/EmissionSimulator.h"
#include "trafficmcmt/emissions/EmissionTypes.h"
#include "trafficmcmt/emissions/GaussianPuffModel.h"
#include "trafficmcmt/emissions/HbefaEmissionModel.h"
#include "trafficmcmt/emissions/PasquillGifford.h"

namespace {

using namespace tmcmt;
using namespace tmcmt::emissions;

constexpr double kOriginLat = 32.606456;
constexpr double kOriginLon = -85.481891;

Timestamp Ms(std::int64_t milliseconds) {
    return Timestamp(milliseconds);
}

EmissionConfig MakeConfig() {
    EmissionConfig config;
    config.enabled = true;
    config.stability = StabilityClass::D;
    config.wind.speed_mps = 3.0;
    config.wind.from_deg = 270.0; // blowing towards the east
    config.puff_interval_s = 1.0;
    config.max_age_s = 60.0;
    config.max_puffs = 500;
    config.grid_hz = 1.0;
    config.grid.has_center = true;
    config.grid.center_latitude = kOriginLat;
    config.grid.center_longitude = kOriginLon;
    config.grid.width_m = 200.0;
    config.grid.height_m = 200.0;
    config.grid.cell_m = 2.0;
    return config;
}

EmissionSource MakeSource(std::uint64_t id, double east, double north, double speed_mps = 0.0,
                          double acceleration_mps2 = 0.0, std::string_view class_name = "car") {
    EmissionSource source;
    source.track_id = id;
    source.position = EnuPoint{east, north};
    source.speed_mps = speed_mps;
    source.acceleration_mps2 = acceleration_mps2;
    source.class_name = class_name;
    return source;
}

// ---------------------------------------------------------------------------
// Pasquill-Gifford
// ---------------------------------------------------------------------------

TEST(PasquillGifford, SigmaYReferenceValues) {
    // The published coefficients are calibrated for x in metres.
    EXPECT_NEAR(SigmaY(StabilityClass::D, 1000.0), 75.3, 0.5);
    EXPECT_NEAR(SigmaY(StabilityClass::A, 1000.0), 187.3, 0.5);
    EXPECT_NEAR(SigmaY(StabilityClass::F, 1000.0), 36.97, 0.2);
}

TEST(PasquillGifford, SigmaZReferenceValues) {
    EXPECT_NEAR(SigmaZ(StabilityClass::D, 1000.0), 29.1, 0.5);
    EXPECT_NEAR(SigmaZ(StabilityClass::A, 1000.0), 347.1, 1.0);
    EXPECT_NEAR(SigmaZ(StabilityClass::F, 1000.0), 9.91, 0.2);
}

TEST(PasquillGifford, SigmaGrowsWithDistance) {
    for (const StabilityClass stability :
         {StabilityClass::A, StabilityClass::B, StabilityClass::C, StabilityClass::D, StabilityClass::E,
          StabilityClass::F}) {
        EXPECT_LT(SigmaY(stability, 100.0), SigmaY(stability, 1000.0));
        EXPECT_LT(SigmaZ(stability, 100.0), SigmaZ(stability, 1000.0));
    }
}

TEST(PasquillGifford, ParseStabilityClass) {
    EXPECT_EQ(ParseStabilityClass("D"), StabilityClass::D);
    EXPECT_EQ(ParseStabilityClass("a"), StabilityClass::A);
    EXPECT_FALSE(ParseStabilityClass("Z").has_value());
    EXPECT_FALSE(ParseStabilityClass("").has_value());
}

TEST(Wind, VectorPointsWhereTheWindBlows) {
    WindConfig fromWest;
    fromWest.speed_mps = 3.0;
    fromWest.from_deg = 270.0;
    const EnuPoint east = fromWest.Vector();
    EXPECT_NEAR(east.east, 3.0, 1e-9);
    EXPECT_NEAR(east.north, 0.0, 1e-9);

    WindConfig fromSouth;
    fromSouth.speed_mps = 2.0;
    fromSouth.from_deg = 180.0;
    const EnuPoint north = fromSouth.Vector();
    EXPECT_NEAR(north.east, 0.0, 1e-9);
    EXPECT_NEAR(north.north, 2.0, 1e-9);
}

// ---------------------------------------------------------------------------
// HBEFA
// ---------------------------------------------------------------------------

TEST(Hbefa, IdleEmissionsArePositive) {
    const HbefaEmissionModel model;
    const EmissionRates rates = model.Rates("car", 0.0, 0.0);
    EXPECT_GT(rates[Pollutant::CO2], 0.0);
    EXPECT_GT(rates[Pollutant::NOx], 0.0);
    EXPECT_GT(rates[Pollutant::CO], 0.0);
    EXPECT_GT(rates[Pollutant::PM10], 0.0);
    EXPECT_NEAR(rates[Pollutant::PM25], rates[Pollutant::PM10] * 0.5, 1e-12);
}

TEST(Hbefa, PedestriansAndBikesEmitNothing) {
    const HbefaEmissionModel model;
    for (const std::string_view name : {"person", "pedestrian", "bike", "bicycle"}) {
        const EmissionRates rates = model.Rates(name, 5.0, 0.0);
        for (const double rate : rates.grams_per_second) {
            EXPECT_DOUBLE_EQ(rate, 0.0) << name;
        }
    }
}

TEST(Hbefa, TruckEmitsMoreNoxThanCar) {
    const HbefaEmissionModel model;
    const EmissionRates car = model.Rates("car", 10.0, 0.0);
    const EmissionRates truck = model.Rates("truck", 10.0, 0.0);
    EXPECT_GT(truck[Pollutant::NOx], car[Pollutant::NOx]);
}

TEST(Hbefa, MatchesReferenceFormulaAtFiftyKmh) {
    const HbefaEmissionModel model;
    // v = 50 km/h, a = 0, P_LDV_7_7 CO2 block.
    const EmissionRates rates = model.Rates("car", 50.0 / 3.6, 0.0);
    const double expected = (2166.094 + 124.9567 * 50.0 - 0.9551173 * 2500.0 + 0.013322 * 125000.0) / 3600.0;
    EXPECT_NEAR(rates[Pollutant::CO2], expected, 1e-9);
}

TEST(Hbefa, CapsSpeedAtTheTableLimit) {
    const HbefaEmissionModel model;
    const EmissionRates at_limit = model.Rates("truck", kMaxHbefaSpeedMps, 0.0);
    const EmissionRates above = model.Rates("truck", 70.0, 0.0);
    EXPECT_DOUBLE_EQ(above[Pollutant::NOx], at_limit[Pollutant::NOx]);
    EXPECT_DOUBLE_EQ(above[Pollutant::CO2], at_limit[Pollutant::CO2]);
}

TEST(Hbefa, NegativeAccelerationPolicy) {
    const HbefaEmissionModel clamped;
    const HbefaEmissionModel strict(0.5, false);

    const EmissionRates idle = clamped.Rates("car", 10.0, 0.0);
    const EmissionRates braking = clamped.Rates("car", 10.0, -2.0);
    EXPECT_DOUBLE_EQ(braking[Pollutant::CO2], idle[Pollutant::CO2]);

    const EmissionRates strictBraking = strict.Rates("car", 10.0, -2.0);
    for (const double rate : strictBraking.grams_per_second) {
        EXPECT_DOUBLE_EQ(rate, 0.0);
    }
}

TEST(Hbefa, AccelerationIncreasesEmissions) {
    const HbefaEmissionModel model;
    const EmissionRates cruise = model.Rates("car", 10.0, 0.0);
    const EmissionRates accelerating = model.Rates("car", 10.0, 2.0);
    EXPECT_GT(accelerating[Pollutant::CO2], cruise[Pollutant::CO2]);
    EXPECT_GT(accelerating[Pollutant::NOx], cruise[Pollutant::NOx]);
}

// ---------------------------------------------------------------------------
// Gaussian puff
// ---------------------------------------------------------------------------

TEST(Puff, AdvectionMatchesAnalyticPosition) {
    PuffState puff;
    puff.origin = EnuPoint{10.0, 20.0};
    puff.center = puff.origin;
    puff.age_s = 10.0;
    puff.PlaceAt(EnuPoint{3.0, -1.0});
    EXPECT_NEAR(puff.center.east, 40.0, 1e-12);
    EXPECT_NEAR(puff.center.north, 10.0, 1e-12);

    PuffState stepped;
    stepped.origin = puff.origin;
    stepped.center = puff.origin;
    for (int i = 0; i < 10; ++i) {
        stepped.Advect(EnuPoint{3.0, -1.0}, 1.0);
    }
    EXPECT_NEAR(stepped.center.east, puff.center.east, 1e-12);
    EXPECT_NEAR(stepped.center.north, puff.center.north, 1e-12);
}

TEST(Puff, DispersionUsesTravelledDistance) {
    PuffState puff;
    puff.age_s = 20.0;
    puff.stability = StabilityClass::D;

    const PuffGeometry slow = ComputePuffGeometry(puff, 1.0);
    const PuffGeometry fast = ComputePuffGeometry(puff, 5.0);

    // 1 m/s * 20 s = 20 m, 5 m/s * 20 s = 100 m.
    EXPECT_NEAR(slow.sigma_y, SigmaY(StabilityClass::D, 20.0), 1e-9);
    EXPECT_NEAR(fast.sigma_y, SigmaY(StabilityClass::D, 100.0), 1e-9);
    EXPECT_GT(fast.sigma_y, slow.sigma_y);
}

TEST(Puff, CalmWindStillDisperses) {
    PuffState puff;
    puff.age_s = 10.0;
    const PuffGeometry geometry = ComputePuffGeometry(puff, 0.0);
    EXPECT_GE(geometry.sigma_y, kMinSigmaM);
    EXPECT_GE(geometry.sigma_z, kMinSigmaM);
    EXPECT_TRUE(geometry.IsValid());
}

TEST(Puff, VerticalSigmaIsCappedByMixingHeight) {
    PuffState puff;
    puff.age_s = 600.0;
    puff.stability = StabilityClass::A;
    const PuffGeometry geometry = ComputePuffGeometry(puff, 5.0, 100.0);
    EXPECT_LE(geometry.sigma_z, 100.0);
}

TEST(Puff, ConcentrationIsSymmetricAcrossTheWindAxis) {
    PuffState puff;
    puff.origin = EnuPoint{0.0, 0.0};
    puff.center = EnuPoint{50.0, 0.0};
    puff.age_s = 10.0;
    puff.source_height_m = 0.5;

    const PuffGeometry geometry = ComputePuffGeometry(puff, 5.0);
    const double left = ConcentrationAt(puff, geometry, 1.0, EnuPoint{50.0, -10.0}, 1.5);
    const double right = ConcentrationAt(puff, geometry, 1.0, EnuPoint{50.0, 10.0}, 1.5);
    EXPECT_NEAR(left, right, 1e-15);
}

TEST(Puff, GroundReflectionDoublesTheGroundLevelTerm) {
    PuffState puff;
    puff.center = EnuPoint{0.0, 0.0};
    puff.age_s = 10.0;
    puff.source_height_m = 0.5;

    const PuffGeometry geometry = ComputePuffGeometry(puff, 5.0);
    const double at_ground = ConcentrationAt(puff, geometry, 1.0, EnuPoint{0.0, 0.0}, 0.0);

    const double two_sz2 = 2.0 * geometry.sigma_z * geometry.sigma_z;
    const double expected =
        1.0 * geometry.inverse_norm * 2.0 * std::exp(-(0.5 * 0.5) / two_sz2);
    EXPECT_NEAR(at_ground, expected, 1e-15);
}

TEST(Puff, TwoDimensionalIntegralMatchesAnalyticValue) {
    PuffState puff;
    puff.center = EnuPoint{0.0, 0.0};
    puff.age_s = 20.0;
    puff.stability = StabilityClass::D;
    puff.source_height_m = 0.5;

    const PuffGeometry geometry = ComputePuffGeometry(puff, 3.0);
    const double z = puff.source_height_m;
    const double radius_x = 6.0 * geometry.sigma_x;
    const double radius_y = 6.0 * geometry.sigma_y;
    const double step = 0.25;

    double integral = 0.0;
    for (double x = -radius_x; x <= radius_x; x += step) {
        for (double y = -radius_y; y <= radius_y; y += step) {
            integral += ConcentrationAt(puff, geometry, 1.0, EnuPoint{x, y}, z) * step * step;
        }
    }

    const double reflection = std::exp(-2.0 * puff.source_height_m * puff.source_height_m /
                                      (geometry.sigma_z * geometry.sigma_z));
    const double analytic = (1.0 / (std::sqrt(2.0 * std::numbers::pi) * geometry.sigma_z)) * (1.0 + reflection);
    EXPECT_NEAR(integral, analytic, analytic * 0.01);
}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

TEST(GridSpec, IndexingClampsOutsideTheRaster) {
    GridSpec spec;
    spec.south_west = EnuPoint{-10.0, -10.0};
    spec.cell_x_m = 5.0;
    spec.cell_y_m = 5.0;
    spec.cols = 4;
    spec.rows = 3;

    EXPECT_EQ(spec.Column(-10.0), 0);
    EXPECT_EQ(spec.Column(-9.9), 0);
    EXPECT_EQ(spec.Column(9.9), 3);
    EXPECT_EQ(spec.Column(-10.1), -1);
    EXPECT_EQ(spec.Column(10.1), 4);
    EXPECT_EQ(spec.Row(-10.0), 0);
    EXPECT_EQ(spec.Row(4.9), 2);
    EXPECT_EQ(spec.Row(5.1), 3);
    EXPECT_NEAR(spec.CellCenterEast(0), -7.5, 1e-12);
    EXPECT_NEAR(spec.CellCenterNorth(0), -7.5, 1e-12);
}

TEST(Grid, AddsPuffContributionToTheCentreCell) {
    GridSpec spec;
    spec.south_west = EnuPoint{-20.0, -20.0};
    spec.cell_x_m = 1.0;
    spec.cell_y_m = 1.0;
    spec.cols = 40;
    spec.rows = 40;
    ConcentrationGrid grid(spec);

    PuffState puff;
    puff.center = EnuPoint{0.0, 0.0};
    puff.age_s = 10.0;
    puff.source_height_m = 0.5;
    puff.mass_g[static_cast<std::size_t>(Pollutant::NOx)] = 1.0;

    const PuffGeometry geometry = ComputePuffGeometry(puff, 3.0);
    grid.AddPuff(puff, geometry, 1.5);

    const std::vector<float>& field = grid.Field(Pollutant::NOx);
    const std::size_t centre = static_cast<std::size_t>(20) * 40 + 20;
    EXPECT_GT(field[centre], 0.f);

    const EnuPoint cell_centre{spec.CellCenterEast(20), spec.CellCenterNorth(20)};
    const double expected = ConcentrationAt(puff, geometry, 1.0, cell_centre, 1.5) * 1.0e6;
    EXPECT_NEAR(field[centre], expected, expected * 1e-4);
}

TEST(Grid, WidensSubCellPuffsToTheCellFootprint) {
    GridSpec spec;
    spec.south_west = EnuPoint{-25.0, -25.0};
    spec.cell_x_m = 5.0;
    spec.cell_y_m = 5.0;
    spec.cols = 10;
    spec.rows = 10;
    ConcentrationGrid grid(spec);

    PuffState puff;
    puff.center = EnuPoint{2.5, 2.5}; // centred on cell (5, 5)
    puff.age_s = 0.0;                 // sigma is still at the kMinSigmaM floor
    puff.source_height_m = 0.5;
    puff.mass_g[static_cast<std::size_t>(Pollutant::NOx)] = 1.0;

    const PuffGeometry geometry = ComputePuffGeometry(puff, 3.0);
    ASSERT_LT(geometry.sigma_y, 0.5 * spec.cell_x_m);

    grid.AddPuff(puff, geometry, 1.5);

    // The expected value uses sigmas clamped to half a cell.
    PuffGeometry fitted = geometry;
    fitted.sigma_x = 2.5;
    fitted.sigma_y = 2.5;
    fitted.sigma_z = 2.5;
    const double norm = std::pow(2.0 * std::numbers::pi, 1.5) * fitted.sigma_x * fitted.sigma_y *
                        fitted.sigma_z;
    fitted.inverse_norm = 1.0 / norm;

    const std::vector<float>& field = grid.Field(Pollutant::NOx);
    const std::size_t centre = static_cast<std::size_t>(5) * 10 + 5;
    const EnuPoint cell_centre{spec.CellCenterEast(5), spec.CellCenterNorth(5)};
    const double expected = ConcentrationAt(puff, fitted, 1.0, cell_centre, 1.5) * 1.0e6;
    EXPECT_NEAR(field[centre], expected, expected * 1e-4);

    // Without the clamp this puff would be ~100x larger.
    const double unclamped = ConcentrationAt(puff, geometry, 1.0, cell_centre, 1.5) * 1.0e6;
    EXPECT_LT(field[centre], unclamped);
}

TEST(Grid, PercentileIgnoresEmptyBackground) {
    GridSpec spec;
    spec.south_west = EnuPoint{-10.0, -10.0};
    spec.cell_x_m = 1.0;
    spec.cell_y_m = 1.0;
    spec.cols = 20;
    spec.rows = 20;
    ConcentrationGrid grid(spec);

    EXPECT_FLOAT_EQ(grid.Stats()[static_cast<std::size_t>(Pollutant::NOx)].p95, 0.f);

    PuffState puff;
    puff.center = EnuPoint{0.0, 0.0};
    puff.age_s = 10.0;
    puff.source_height_m = 0.5;
    puff.mass_g[static_cast<std::size_t>(Pollutant::NOx)] = 1.0;
    grid.AddPuff(puff, ComputePuffGeometry(puff, 3.0), 1.5);

    const EmissionFieldStats stats = grid.Stats()[static_cast<std::size_t>(Pollutant::NOx)];
    EXPECT_GT(stats.max, 0.f);
    EXPECT_GT(stats.p95, 0.f);
    EXPECT_LE(stats.p95, stats.max);
    EXPECT_LT(stats.mean, stats.p95);
}

// ---------------------------------------------------------------------------
// Simulator
// ---------------------------------------------------------------------------

TEST(Simulator, SpawnsPuffsAndBuildsDownwindField) {
    EmissionSimulator simulator(MakeConfig(), GeoPoint{kOriginLat, kOriginLon});
    const std::vector<EmissionSource> sources{MakeSource(1, 0.0, 0.0)};

    for (int second = 0; second <= 20; ++second) {
        simulator.Step(Ms(static_cast<std::int64_t>(second) * 1000), sources);
    }

    EXPECT_GT(simulator.PuffCount(), 0u);
    EXPECT_EQ(simulator.EmitterCount(), 1u);

    const std::optional<EmissionGridSnapshot> snapshot = simulator.BuildSnapshot(Ms(20000));
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->cols, 100);
    EXPECT_EQ(snapshot->rows, 100);

    const std::vector<float>& nox = snapshot->fields[static_cast<std::size_t>(Pollutant::NOx)];
    ASSERT_EQ(nox.size(), static_cast<std::size_t>(100 * 100));

    double east_mass = 0.0;
    double west_mass = 0.0;
    for (int row = 0; row < snapshot->rows; ++row) {
        for (int col = 0; col < snapshot->cols; ++col) {
            const double value = nox[static_cast<std::size_t>(row) * 100 + col];
            if (col > 50) {
                east_mass += value;
            } else if (col < 50) {
                west_mass += value;
            }
        }
    }

    EXPECT_GT(snapshot->stats[static_cast<std::size_t>(Pollutant::NOx)].max, 0.f);
    EXPECT_GT(east_mass, west_mass * 10.0);
}

TEST(Simulator, ExpiresOldPuffs) {
    EmissionConfig config = MakeConfig();
    config.max_age_s = 2.0;
    EmissionSimulator simulator(config, GeoPoint{kOriginLat, kOriginLon});
    const std::vector<EmissionSource> sources{MakeSource(1, 0.0, 0.0)};

    simulator.Step(Ms(0), sources);
    simulator.Step(Ms(1000), sources);
    EXPECT_GT(simulator.PuffCount(), 0u);

    simulator.Step(Ms(5000), sources);
    // The puff released at 1 s is older than 2 s and must be gone.
    EXPECT_EQ(simulator.PuffCount(), 1u);
}

TEST(Simulator, DropsEmittersThatDisappear) {
    EmissionSimulator simulator(MakeConfig(), GeoPoint{kOriginLat, kOriginLon});
    simulator.Step(Ms(0), {MakeSource(1, 0.0, 0.0), MakeSource(2, 5.0, 0.0)});
    EXPECT_EQ(simulator.EmitterCount(), 2u);

    simulator.Step(Ms(1000), {MakeSource(1, 0.0, 0.0)});
    EXPECT_EQ(simulator.EmitterCount(), 1u);
}

TEST(Simulator, GridFrameRate) {
    EmissionConfig config = MakeConfig();
    config.grid_hz = 2.0;
    EmissionSimulator simulator(config, GeoPoint{kOriginLat, kOriginLon});
    const std::vector<EmissionSource> sources{MakeSource(1, 0.0, 0.0)};

    simulator.Step(Ms(0), sources);
    EXPECT_TRUE(simulator.GridDue(Ms(0)));

    ASSERT_TRUE(simulator.BuildSnapshot(Ms(0)).has_value());
    EXPECT_FALSE(simulator.GridDue(Ms(499)));
    EXPECT_TRUE(simulator.GridDue(Ms(500)));
}

TEST(Simulator, SnapshotBoundsMatchTheConfiguredGrid) {
    EmissionSimulator simulator(MakeConfig(), GeoPoint{kOriginLat, kOriginLon});
    const std::optional<EmissionGridSnapshot> snapshot = simulator.BuildSnapshot(Ms(0));
    ASSERT_TRUE(snapshot.has_value());

    const GeoPoint centre = EnuToGeographic(EnuPoint{0.0, 0.0}, GeoPoint{kOriginLat, kOriginLon});
    EXPECT_NEAR(0.5 * (snapshot->south_west.latitude + snapshot->north_east.latitude), centre.latitude, 1e-7);
    EXPECT_NEAR(0.5 * (snapshot->south_west.longitude + snapshot->north_east.longitude), centre.longitude, 1e-7);
    EXPECT_GT(snapshot->north_east.latitude, snapshot->south_west.latitude);
    EXPECT_GT(snapshot->north_east.longitude, snapshot->south_west.longitude);
}

TEST(Simulator, InjectedModelControlsRates) {
    class ConstantModel final : public IEmissionModel {
    public:
        EmissionRates Rates(std::string_view, double, double) const override {
            EmissionRates rates;
            rates[Pollutant::NOx] = 1.0; // 1 g/s
            return rates;
        }
    };

    EmissionSimulator simulator(MakeConfig(), GeoPoint{kOriginLat, kOriginLon},
                                std::make_unique<ConstantModel>());
    const std::vector<EmissionSource> sources{MakeSource(1, 0.0, 0.0)};

    simulator.Step(Ms(0), sources);
    const std::optional<EmissionGridSnapshot> snapshot = simulator.BuildSnapshot(Ms(0));
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_GT(snapshot->stats[static_cast<std::size_t>(Pollutant::NOx)].max, 0.f);
    EXPECT_EQ(snapshot->stats[static_cast<std::size_t>(Pollutant::CO2)].max, 0.f);
}

} // namespace
