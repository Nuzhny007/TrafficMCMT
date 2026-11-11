#include "trafficmcmt/io/ResultSinks.h"

#include <filesystem>
#include <system_error>
#include <utility>

#include "trafficmcmt/io/TrackletCodec.h"

namespace tmcmt {
namespace {

Status PrepareFile(const std::string& path, std::ofstream& stream) {
    if (path.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "result sink path must not be empty");
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
        return Status::Error(ErrorCode::IoError, "cannot open result file: " + path);
    }
    return Status::Ok();
}

Status FlushStream(std::ofstream& stream, const std::string& path) {
    if (!stream.is_open()) {
        return Status::Ok();
    }
    stream.flush();
    if (!stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot flush " + path);
    }
    return Status::Ok();
}

} // namespace

JsonlResultSink::JsonlResultSink(std::string path, std::string mergesPath)
    : m_path(std::move(path)), m_mergesPath(std::move(mergesPath)) {}

JsonlResultSink::~JsonlResultSink() {
    if (m_stream.is_open()) {
        m_stream.flush();
    }
    if (m_merges.is_open()) {
        m_merges.flush();
    }
}

Result<std::unique_ptr<JsonlResultSink>> JsonlResultSink::Open(const std::string& path) {
    std::unique_ptr<JsonlResultSink> sink(new JsonlResultSink(path, path + ".merges.jsonl"));

    Status status = PrepareFile(sink->m_path, sink->m_stream);
    if (!status.ok()) {
        return status;
    }
    status = PrepareFile(sink->m_mergesPath, sink->m_merges);
    if (!status.ok()) {
        return status;
    }
    return sink;
}

Status JsonlResultSink::Publish(const GlobalTrackUpdate& update) {
    if (!m_stream.is_open()) {
        return Status::Error(ErrorCode::IoError, "jsonl result sink is not open: " + m_path);
    }

    auto json = GlobalTrackCodec::EncodeJson(update);
    if (!json) {
        return json.status();
    }
    m_stream << json.value() << '\n';
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot write to " + m_path);
    }
    ++m_updates;
    return Status::Ok();
}

Status JsonlResultSink::PublishMerge(const GlobalIdMergeEvent& event) {
    if (!m_merges.is_open()) {
        return Status::Error(ErrorCode::IoError, "jsonl merge sink is not open: " + m_mergesPath);
    }

    auto json = GlobalIdMergeCodec::EncodeJson(event);
    if (!json) {
        return json.status();
    }
    m_merges << json.value() << '\n';
    if (!m_merges.good()) {
        return Status::Error(ErrorCode::IoError, "cannot write to " + m_mergesPath);
    }
    ++m_mergeCount;
    return Status::Ok();
}

Status JsonlResultSink::Flush() {
    Status status = FlushStream(m_stream, m_path);
    if (!status.ok()) {
        return status;
    }
    return FlushStream(m_merges, m_mergesPath);
}

std::string JsonlResultSink::Describe() const {
    return "jsonl-results:" + m_path;
}

} // namespace tmcmt
