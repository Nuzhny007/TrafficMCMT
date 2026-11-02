#include "trafficmcmt/io/FileTrackletSource.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include "trafficmcmt/io/TrackletCodec.h"

namespace tmcmt {
namespace {

std::string LowercaseExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

TrackletFileFormat DetectByExtension(const std::string& path) {
    const std::string extension = LowercaseExtension(path);
    if (extension == ".jsonl" || extension == ".json") {
        return TrackletFileFormat::Jsonl;
    }
    if (extension == ".bin" || extension == ".pb" || extension == ".protobuf" || extension == ".tracklets") {
        return TrackletFileFormat::Protobuf;
    }
    return TrackletFileFormat::Auto;
}

Result<std::uint64_t> ReadVarint(std::istream& stream) {
    std::uint64_t value = 0;
    int shift = 0;
    while (shift <= 63) {
        char raw = 0;
        if (!stream.get(raw)) {
            return Status::Error(ErrorCode::ParseError, "truncated varint length prefix");
        }
        const auto byte = static_cast<unsigned char>(raw);
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return value;
        }
        shift += 7;
    }
    return Status::Error(ErrorCode::ParseError, "malformed varint length prefix");
}

bool IsBlank(const std::string& line) {
    return std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

} // namespace

FileTrackletSource::FileTrackletSource(std::string path, TrackletFileFormat format, double speed)
    : m_path(std::move(path)), m_format(format), m_speed(speed) {}

FileTrackletSource::~FileTrackletSource() = default;

Result<std::unique_ptr<FileTrackletSource>> FileTrackletSource::Open(const std::string& path,
                                                                     const Options& options) {
    if (path.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "tracklet file path must not be empty");
    }
    if (options.speed < 0.0) {
        return Status::Error(ErrorCode::InvalidArgument, "replay speed must be >= 0");
    }

    TrackletFileFormat format = options.format;
    if (format == TrackletFileFormat::Auto) {
        format = DetectByExtension(path);
    }

    std::unique_ptr<FileTrackletSource> source(new FileTrackletSource(path, format, options.speed));
    const Status status = source->OpenStream();
    if (!status.ok()) {
        return status;
    }
    return source;
}

Status FileTrackletSource::OpenStream() {
    m_stream.close();
    m_stream.clear();
    m_stream.open(m_path, std::ios::binary);
    if (!m_stream.is_open()) {
        return Status::Error(ErrorCode::IoError, "cannot open tracklet file: " + m_path);
    }

    if (m_format == TrackletFileFormat::Auto) {
        // Sniff the first non-whitespace byte: JSON objects start with '{',
        // protobuf messages almost never do.
        int ch = m_stream.peek();
        while (ch != std::char_traits<char>::eof() && std::isspace(static_cast<unsigned char>(ch)) != 0) {
            m_stream.get();
            ch = m_stream.peek();
        }
        m_format = (ch == '{') ? TrackletFileFormat::Jsonl : TrackletFileFormat::Protobuf;
        m_stream.clear();
        m_stream.seekg(0, std::ios::beg);
    }

    m_pacingInitialized = false;
    m_firstTimestamp.reset();
    m_lastPacingMs = 0;
    return Status::Ok();
}

Status FileTrackletSource::Reset() {
    if (!m_stream.is_open()) {
        return OpenStream();
    }
    m_stream.clear();
    m_stream.seekg(0, std::ios::beg);
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot rewind tracklet file: " + m_path);
    }
    m_pacingInitialized = false;
    m_firstTimestamp.reset();
    m_lastPacingMs = 0;
    return Status::Ok();
}

Result<bool> FileTrackletSource::Next(Tracklet& out) {
    Result<bool> read = (m_format == TrackletFileFormat::Jsonl) ? ReadJsonLine(out) : ReadFramed(out);
    if (!read || !read.value()) {
        return read;
    }
    ApplyPacing(out);
    return true;
}

Result<bool> FileTrackletSource::ReadJsonLine(Tracklet& out) {
    std::string line;
    while (std::getline(m_stream, line)) {
        if (IsBlank(line)) {
            continue;
        }
        auto tracklet = TrackletCodec::DecodeJson(line);
        if (!tracklet) {
            return tracklet.status();
        }
        out = std::move(tracklet).value();
        return true;
    }
    if (m_stream.bad()) {
        return Status::Error(ErrorCode::IoError, "I/O error while reading tracklet file: " + m_path);
    }
    return false;
}

Result<bool> FileTrackletSource::ReadFramed(Tracklet& out) {
    if (m_stream.peek() == std::char_traits<char>::eof()) {
        return false;
    }

    auto length = ReadVarint(m_stream);
    if (!length) {
        return length.status();
    }
    const std::uint64_t size = length.value();
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Status::Error(ErrorCode::ParseError, "framed message length does not fit into size_t");
    }

    std::string payload(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        m_stream.read(payload.data(), static_cast<std::streamsize>(size));
        if (static_cast<std::uint64_t>(m_stream.gcount()) != size) {
            return Status::Error(ErrorCode::ParseError, "truncated framed protobuf message in " + m_path);
        }
    }

    auto tracklet = TrackletCodec::DecodeBinary(payload);
    if (!tracklet) {
        return tracklet.status();
    }
    out = std::move(tracklet).value();
    return true;
}

void FileTrackletSource::ApplyPacing(const Tracklet& tracklet) {
    if (!(m_speed > 0.0)) {
        return;
    }

    if (!m_pacingInitialized) {        m_pacingInitialized = true;
        m_firstTimestamp = tracklet.begin;
        m_lastPacingMs = 0;
        return;
    }

    const std::int64_t relativeMs = (tracklet.begin - *m_firstTimestamp).count();
    const auto targetMs = static_cast<std::int64_t>(static_cast<double>(relativeMs) / m_speed);
    const std::int64_t delayMs = targetMs - m_lastPacingMs;
    if (delayMs <= 0) {
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    m_lastPacingMs = targetMs;
}

std::string FileTrackletSource::Describe() const {
    return "file:" + m_path + " (" + ToString(m_format) + ", speed=" + std::to_string(m_speed) + ")";
}

} // namespace tmcmt
