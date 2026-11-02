#include "trafficmcmt/io/TrackletSinks.h"

#include <filesystem>
#include <system_error>
#include <utility>

#include "trafficmcmt/io/TrackletCodec.h"

namespace tmcmt {
namespace {

Status PrepareFile(const std::string& path, std::ofstream& stream) {
    if (path.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "sink path must not be empty");
    }

    const std::filesystem::path filePath(path);
    const std::filesystem::path parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return Status::Error(ErrorCode::IoError,
                                 "cannot create output directory '" + parent.string() + "': " + ec.message());
        }
    }

    stream.open(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return Status::Error(ErrorCode::IoError, "cannot open output file: " + path);
    }
    return Status::Ok();
}

} // namespace

// ---------------------------------------------------------------------------
// JsonlTrackletSink
// ---------------------------------------------------------------------------

JsonlTrackletSink::JsonlTrackletSink(std::string path, bool pretty) : m_path(std::move(path)), m_pretty(pretty) {}

JsonlTrackletSink::~JsonlTrackletSink() {
    if (m_stream.is_open()) {
        m_stream.flush();
    }
}

Result<std::unique_ptr<JsonlTrackletSink>> JsonlTrackletSink::Open(const std::string& path, bool pretty) {
    std::unique_ptr<JsonlTrackletSink> sink(new JsonlTrackletSink(path, pretty));
    const Status status = PrepareFile(path, sink->m_stream);
    if (!status.ok()) {
        return status;
    }
    return sink;
}

Status JsonlTrackletSink::Write(const Tracklet& tracklet) {
    if (!m_stream.is_open()) {
        return Status::Error(ErrorCode::IoError, "jsonl sink is not open: " + m_path);
    }

    auto json = TrackletCodec::EncodeJson(tracklet, m_pretty);
    if (!json) {
        return json.status();
    }
    m_stream << json.value() << '\n';
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot write to " + m_path);
    }
    return Status::Ok();
}

Status JsonlTrackletSink::Flush() {
    if (!m_stream.is_open()) {
        return Status::Ok();
    }
    m_stream.flush();
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot flush " + m_path);
    }
    return Status::Ok();
}

std::string JsonlTrackletSink::Describe() const {
    return "jsonl:" + m_path;
}

// ---------------------------------------------------------------------------
// BinaryTrackletSink
// ---------------------------------------------------------------------------

BinaryTrackletSink::BinaryTrackletSink(std::string path, bool quantize)
    : m_path(std::move(path)), m_quantize(quantize) {}

BinaryTrackletSink::~BinaryTrackletSink() {
    if (m_stream.is_open()) {
        m_stream.flush();
    }
}

Result<std::unique_ptr<BinaryTrackletSink>> BinaryTrackletSink::Open(const std::string& path,
                                                                     bool quantize_embeddings) {
    std::unique_ptr<BinaryTrackletSink> sink(new BinaryTrackletSink(path, quantize_embeddings));
    const Status status = PrepareFile(path, sink->m_stream);
    if (!status.ok()) {
        return status;
    }
    return sink;
}

Status BinaryTrackletSink::Write(const Tracklet& tracklet) {
    if (!m_stream.is_open()) {
        return Status::Error(ErrorCode::IoError, "binary sink is not open: " + m_path);
    }

    TrackletCodecOptions options;
    options.quantize_embeddings = m_quantize;

    auto framed = TrackletCodec::EncodeFramed(tracklet, options);
    if (!framed) {
        return framed.status();
    }
    m_stream.write(framed.value().data(), static_cast<std::streamsize>(framed.value().size()));
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot write to " + m_path);
    }
    return Status::Ok();
}

Status BinaryTrackletSink::Flush() {
    if (!m_stream.is_open()) {
        return Status::Ok();
    }
    m_stream.flush();
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot flush " + m_path);
    }
    return Status::Ok();
}

std::string BinaryTrackletSink::Describe() const {
    return "binary:" + m_path + (m_quantize ? " (int8)" : " (float32)");
}

} // namespace tmcmt
