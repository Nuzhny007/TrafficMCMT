#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include "trafficmcmt/bev/WorldTrajectoryFusion.h"
#include "trafficmcmt/core/GlobalTrack.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/io/IResultSink.h"

namespace tmcmt::bev {

enum class WorldOutputFormat : std::uint8_t {
    Off = 0,
    GeoJson = 1,
    Csv = 2,
};

[[nodiscard]] const char* ToString(WorldOutputFormat format) noexcept;
[[nodiscard]] WorldOutputFormat ParseWorldOutputFormat(std::string_view text) noexcept;

/// Streams fused world trajectories as newline-delimited GeoJSON features or
/// as CSV rows.
///
/// One record is written per kept sample. Overlap duplicates are removed by
/// `WorldTrajectoryFusion` before anything reaches the file, so the output is
/// already de-duplicated. GeoJSON coordinates are WGS-84 `[lon, lat]` when the
/// trajectory carries an origin and local ENU `[east, north]` otherwise; the
/// `frame` property always states which one is used.
class WorldTrackSink final : public IResultSink {
public:
    [[nodiscard]] static Result<std::unique_ptr<WorldTrackSink>> Open(
        const std::string& path, WorldOutputFormat format,
        WorldFusionOptions options = {});

    ~WorldTrackSink() override;

    [[nodiscard]] Status Publish(const GlobalTrackUpdate& update) override;
    [[nodiscard]] Status PublishMerge(const GlobalIdMergeEvent& event) override;
    [[nodiscard]] Status Flush() override;
    [[nodiscard]] std::string Describe() const override;

    [[nodiscard]] const std::string& path() const noexcept {
        return m_path;
    }
    [[nodiscard]] WorldOutputFormat format() const noexcept {
        return m_format;
    }
    [[nodiscard]] std::uint64_t records() const noexcept {
        return m_records;
    }
    [[nodiscard]] std::uint64_t trajectories() const noexcept {
        return m_trajectories;
    }
    [[nodiscard]] FusionStats Stats() const noexcept {
        return m_fusion.Stats();
    }

private:
    WorldTrackSink(std::string path, WorldOutputFormat format, WorldFusionOptions options);

    [[nodiscard]] Status WriteGeoJson(const WorldTrajectory& trajectory);
    [[nodiscard]] Status WriteCsv(const WorldTrajectory& trajectory);
    [[nodiscard]] Status WriteCsvHeader();

    std::string m_path;
    WorldOutputFormat m_format = WorldOutputFormat::Off;
    WorldTrajectoryFusion m_fusion;
    std::ofstream m_stream;
    std::uint64_t m_records = 0;
    std::uint64_t m_trajectories = 0;
    bool m_headerWritten = false;
};

} // namespace tmcmt::bev
