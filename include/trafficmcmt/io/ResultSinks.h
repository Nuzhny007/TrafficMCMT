#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "trafficmcmt/core/GlobalTrack.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/io/IResultSink.h"

namespace tmcmt {

/// Writes one protobuf-JSON object per line.
///
/// Global track updates go to `path`; identity merge events go to
/// `path + ".merges.jsonl"` so that a consumer can decode both streams without
/// a wrapper envelope. `Flush()` fsyncs both files.
class JsonlResultSink final : public IResultSink {
public:
    [[nodiscard]] static Result<std::unique_ptr<JsonlResultSink>> Open(const std::string& path);
    ~JsonlResultSink() override;

    [[nodiscard]] Status Publish(const GlobalTrackUpdate& update) override;
    [[nodiscard]] Status PublishMerge(const GlobalIdMergeEvent& event) override;
    [[nodiscard]] Status Flush() override;
    [[nodiscard]] std::string Describe() const override;

    [[nodiscard]] const std::string& merges_path() const noexcept {
        return m_mergesPath;
    }

    [[nodiscard]] std::uint64_t updates() const noexcept {
        return m_updates;
    }

    [[nodiscard]] std::uint64_t merges() const noexcept {
        return m_mergeCount;
    }

private:
    JsonlResultSink(std::string path, std::string mergesPath);

    std::string m_path;
    std::string m_mergesPath;
    std::ofstream m_stream;
    std::ofstream m_merges;
    std::uint64_t m_updates = 0;
    std::uint64_t m_mergeCount = 0;
};

} // namespace tmcmt
