#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/io/ITrackletSource.h"

namespace tmcmt {

/// Replays a JSONL or length-delimited protobuf tracklet file.
///
/// Format detection:
///   * `format != Auto` — use the configured format;
///   * extension `.jsonl` / `.json` — JSONL;
///   * extension `.bin` / `.pb` / `.protobuf` / `.tracklets` — protobuf;
///   * otherwise the first non-whitespace byte decides: `{` -> JSONL.
class FileTrackletSource final : public ITrackletSource {
public:
    struct Options {
        TrackletFileFormat format = TrackletFileFormat::Auto;
        /// 1.0 = real time, 0 = no pacing (used by tests and offline runs).
        double speed = 1.0;
    };

    [[nodiscard]] static Result<std::unique_ptr<FileTrackletSource>> Open(const std::string& path,
                                                                         const Options& options = {});

    ~FileTrackletSource() override;

    [[nodiscard]] Result<bool> Next(Tracklet& out) override;
    [[nodiscard]] Status Reset() override;
    [[nodiscard]] std::string Describe() const override;

    [[nodiscard]] TrackletFileFormat Format() const noexcept {
        return m_format;
    }

private:
    FileTrackletSource(std::string path, TrackletFileFormat format, double speed);

    [[nodiscard]] Status OpenStream();
    [[nodiscard]] Result<bool> ReadJsonLine(Tracklet& out);
    [[nodiscard]] Result<bool> ReadFramed(Tracklet& out);
    void ApplyPacing(const Tracklet& tracklet);

    std::string m_path;
    TrackletFileFormat m_format = TrackletFileFormat::Auto;
    double m_speed = 1.0;
    std::ifstream m_stream;
    std::optional<Timestamp> m_firstTimestamp;
    std::int64_t m_lastPacingMs = 0;
    bool m_pacingInitialized = false;
};

} // namespace tmcmt
