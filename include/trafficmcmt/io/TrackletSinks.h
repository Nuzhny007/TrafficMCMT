#pragma once

#include <fstream>
#include <memory>
#include <string>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/io/ITrackletSink.h"

namespace tmcmt {

/// Writes one JSON object per line (protobuf JSON mapping).
class JsonlTrackletSink final : public ITrackletSink {
public:
    [[nodiscard]] static Result<std::unique_ptr<JsonlTrackletSink>> Open(const std::string& path,
                                                                         bool pretty = false);
    ~JsonlTrackletSink() override;

    [[nodiscard]] Status Write(const Tracklet& tracklet) override;
    [[nodiscard]] Status Flush() override;
    [[nodiscard]] std::string Describe() const override;

private:
    JsonlTrackletSink(std::string path, bool pretty);

    std::string m_path;
    bool m_pretty = false;
    std::ofstream m_stream;
};

/// Writes concatenated varint-length-delimited protobuf messages.
class BinaryTrackletSink final : public ITrackletSink {
public:
    [[nodiscard]] static Result<std::unique_ptr<BinaryTrackletSink>> Open(const std::string& path,
                                                                         bool quantize_embeddings = false);
    ~BinaryTrackletSink() override;

    [[nodiscard]] Status Write(const Tracklet& tracklet) override;
    [[nodiscard]] Status Flush() override;
    [[nodiscard]] std::string Describe() const override;

private:
    BinaryTrackletSink(std::string path, bool quantize);

    std::string m_path;
    bool m_quantize = false;
    std::ofstream m_stream;
};

} // namespace tmcmt
