#include "trafficmcmt/bev/WorldTrackSink.h"

#include <cctype>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::bev {
namespace {

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += character;
            break;
        }
    }
    return out;
}

std::string CsvEscape(std::string_view text) {
    if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(text);
    }
    std::string out = "\"";
    for (const char character : text) {
        if (character == '"') {
            out += "\"\"";
        } else {
            out += character;
        }
    }
    out += '"';
    return out;
}

std::string Number(double value, int precision = 9) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(precision) << value;
    return out.str();
}

} // namespace

const char* ToString(WorldOutputFormat format) noexcept {
    switch (format) {
    case WorldOutputFormat::Off:
        return "off";
    case WorldOutputFormat::GeoJson:
        return "geojson";
    case WorldOutputFormat::Csv:
        return "csv";
    }
    return "off";
}

WorldOutputFormat ParseWorldOutputFormat(std::string_view text) noexcept {
    std::string lower;
    lower.reserve(text.size());
    for (const char character : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    if (lower == "geojson" || lower == "geojsonl" || lower == "jsonl") {
        return WorldOutputFormat::GeoJson;
    }
    if (lower == "csv") {
        return WorldOutputFormat::Csv;
    }
    return WorldOutputFormat::Off;
}

Result<std::unique_ptr<WorldTrackSink>> WorldTrackSink::Open(const std::string& path,
                                                             WorldOutputFormat format,
                                                             WorldFusionOptions options) {
    if (path.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "world output path is empty");
    }
    if (format == WorldOutputFormat::Off) {
        return Status::Error(ErrorCode::InvalidArgument, "world output format is off");
    }

    auto sink = std::unique_ptr<WorldTrackSink>(new WorldTrackSink(path, format, options));
    sink->m_stream.open(path, std::ios::binary | std::ios::trunc);
    if (!sink->m_stream.is_open()) {
        return Status::Error(ErrorCode::IoError, "cannot open world output: " + path);
    }
    if (format == WorldOutputFormat::Csv) {
        Status status = sink->WriteCsvHeader();
        if (!status.ok()) {
            return status;
        }
    }
    return sink;
}

WorldTrackSink::WorldTrackSink(std::string path, WorldOutputFormat format, WorldFusionOptions options)
    : m_path(std::move(path)), m_format(format), m_fusion(options) {}

WorldTrackSink::~WorldTrackSink() {
    if (m_stream.is_open()) {
        m_stream.flush();
        m_stream.close();
    }
}

Status WorldTrackSink::Publish(const GlobalTrackUpdate& update) {
    if (!m_stream.is_open()) {
        return Status::Error(ErrorCode::IoError, "world output is not open: " + m_path);
    }

    const WorldTrajectory trajectory = m_fusion.Fuse(update);
    if (trajectory.samples.empty()) {
        return Status::Ok();
    }
    ++m_trajectories;

    Status status = m_format == WorldOutputFormat::Csv ? WriteCsv(trajectory)
                                                       : WriteGeoJson(trajectory);
    if (!status.ok()) {
        return status;
    }
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot write world output: " + m_path);
    }
    return Status::Ok();
}

Status WorldTrackSink::PublishMerge(const GlobalIdMergeEvent& event) {
    (void)event;
    return Status::Ok();
}

Status WorldTrackSink::Flush() {
    if (!m_stream.is_open()) {
        return Status::Ok();
    }
    m_stream.flush();
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot flush world output: " + m_path);
    }
    return Status::Ok();
}

std::string WorldTrackSink::Describe() const {
    return "world-" + std::string(ToString(m_format)) + ":" + m_path;
}

Status WorldTrackSink::WriteCsvHeader() {
    if (m_headerWritten) {
        return Status::Ok();
    }
    m_stream << "global_id,ts_ms,tracklet_id,camera,type,east,north,latitude,longitude,"
                "speed_mps,course_deg,is_final,trajectory_begin_ms,trajectory_end_ms,"
                "duplicates_suppressed\n";
    m_headerWritten = true;
    if (!m_stream.good()) {
        return Status::Error(ErrorCode::IoError, "cannot write world output header: " + m_path);
    }
    return Status::Ok();
}

Status WorldTrackSink::WriteCsv(const WorldTrajectory& trajectory) {
    for (const WorldSample& sample : trajectory.samples) {
        const GeoPoint geo = trajectory.has_origin
                                 ? EnuToGeographic(sample.position, trajectory.origin)
                                 : GeoPoint{};
        m_stream << trajectory.global_id << ',' << sample.ts.count() << ',' << sample.tracklet_id << ','
                 << CsvEscape(sample.camera) << ',' << CsvEscape(ToString(sample.type)) << ','
                 << Number(sample.position.east) << ',' << Number(sample.position.north) << ',';
        if (trajectory.has_origin) {
            m_stream << Number(geo.latitude) << ',' << Number(geo.longitude);
        } else {
            m_stream << ',';
        }
        m_stream << ',' << Number(trajectory.speed_mps) << ',' << Number(trajectory.course_deg) << ','
                 << (trajectory.is_final ? "true" : "false") << ',' << trajectory.begin.count() << ','
                 << trajectory.end.count() << ',' << trajectory.duplicates_suppressed << '\n';
        ++m_records;
    }
    return Status::Ok();
}

Status WorldTrackSink::WriteGeoJson(const WorldTrajectory& trajectory) {
    const char* frame = trajectory.has_origin ? "wgs84" : "enu";
    for (const WorldSample& sample : trajectory.samples) {
        double first = sample.position.east;
        double second = sample.position.north;
        if (trajectory.has_origin) {
            const GeoPoint geo = EnuToGeographic(sample.position, trajectory.origin);
            first = geo.longitude;
            second = geo.latitude;
        }

        m_stream << "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":["
                 << Number(first) << ',' << Number(second) << "]},\"properties\":{"
                 << "\"global_id\":" << trajectory.global_id << ",\"tracklet_id\":" << sample.tracklet_id
                 << ",\"camera\":\"" << JsonEscape(sample.camera) << "\""
                 << ",\"type\":\"" << JsonEscape(ToString(sample.type)) << "\""
                 << ",\"ts_ms\":" << sample.ts.count() << ",\"east\":" << Number(sample.position.east)
                 << ",\"north\":" << Number(sample.position.north) << ",\"frame\":\"" << frame << "\""
                 << ",\"speed_mps\":" << Number(trajectory.speed_mps)
                 << ",\"course_deg\":" << Number(trajectory.course_deg)
                 << ",\"is_final\":" << (trajectory.is_final ? "true" : "false")
                 << ",\"trajectory_begin_ms\":" << trajectory.begin.count()
                 << ",\"trajectory_end_ms\":" << trajectory.end.count()
                 << ",\"duplicates_suppressed\":" << trajectory.duplicates_suppressed << "}}\n";
        ++m_records;
    }
    return Status::Ok();
}

} // namespace tmcmt::bev
