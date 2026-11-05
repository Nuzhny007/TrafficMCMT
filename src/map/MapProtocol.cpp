#include "trafficmcmt/map/MapTypes.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace tmcmt::map {
namespace {

void WriteEscaped(std::ostringstream& out, std::string_view text) {
    for (const char ch : text) {
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec << std::setfill(' ');
            } else {
                out << ch;
            }
            break;
        }
    }
}

void WriteNumber(std::ostringstream& out, double value, int precision) {
    if (!std::isfinite(value)) {
        value = 0.0;
    }
    // Avoid "-0.0000000" in the stream.
    if (std::fabs(value) < 0.5 * std::pow(10.0, -precision)) {
        value = 0.0;
    }
    out << std::fixed << std::setprecision(precision) << value;
}

void WriteGeo(std::ostringstream& out, const GeoPoint& point) {
    out << '[';
    WriteNumber(out, point.longitude, 7);
    out << ',';
    WriteNumber(out, point.latitude, 7);
    out << ']';
}

void WriteTrack(std::ostringstream& out, const MapTrackUpdate& track) {
    out << "{\"global_id\":" << track.global_id << ",\"camera_id\":" << track.camera_id << ",\"class\":\"";
    WriteEscaped(out, track.class_name);
    out << "\",\"position\":";
    WriteGeo(out, track.position);
    if (track.has_velocity) {
        out << ",\"velocity\":[";
        WriteNumber(out, track.velocity.east, 2);
        out << ',';
        WriteNumber(out, track.velocity.north, 2);
        out << "],\"speed_kmh\":";
        WriteNumber(out, track.speed_kmh, 2);
    }
    if (!track.trail.empty()) {
        out << ",\"trail\":[";
        bool first = true;
        for (const GeoPoint& point : track.trail) {
            if (!first) {
                out << ',';
            }
            first = false;
            WriteGeo(out, point);
        }
        out << ']';
    }
    out << '}';
}

void WriteZones(std::ostringstream& out, const std::vector<MapCameraZone>& zones) {
    out << '[';
    bool firstZone = true;
    for (const MapCameraZone& zone : zones) {
        if (!firstZone) {
            out << ',';
        }
        firstZone = false;
        out << "{\"camera_id\":" << zone.camera_id << ",\"polygon\":[";
        bool firstPoint = true;
        for (const GeoPoint& point : zone.polygon) {
            if (!firstPoint) {
                out << ',';
            }
            firstPoint = false;
            WriteGeo(out, point);
        }
        out << "],\"color\":[" << zone.color[0] << ',' << zone.color[1] << ',' << zone.color[2] << ','
            << zone.color[3] << "]}";
    }
    out << ']';
}

} // namespace

const char* MapClassName(ObjectType type) noexcept {
    switch (type) {
    case ObjectType::Pedestrian:
        return "person";
    case ObjectType::Car:
        return "car";
    case ObjectType::Truck:
        return "truck";
    case ObjectType::Bus:
        return "bus";
    case ObjectType::Bike:
        return "bike";
    case ObjectType::Unknown:
        break;
    }
    return "unknown";
}

std::string ToJson(const MapFrame& frame) {
    std::ostringstream out;
    out << '{';

    switch (frame.type) {
    case MapFrameType::TracksUpdate:
        out << "\"type\":\"tracks_update\",\"timestamp\":" << frame.timestamp.count() << ",\"mode\":\""
            << (frame.snapshot ? "snapshot" : "delta") << "\",\"tracks\":[";
        for (std::size_t i = 0; i < frame.tracks.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            WriteTrack(out, frame.tracks[i]);
        }
        out << ']';
        break;
    case MapFrameType::CameraZones:
        out << "\"type\":\"camera_zones\",\"timestamp\":" << frame.timestamp.count() << ",\"camera_zones\":";
        WriteZones(out, frame.zones);
        break;
    case MapFrameType::Heartbeat:
        out << "\"type\":\"heartbeat\",\"timestamp\":" << frame.timestamp.count();
        break;
    case MapFrameType::IdMerge:
        out << "\"type\":\"id_merge\",\"timestamp\":" << frame.timestamp.count() << ",\"old_id\":" << frame.old_id
            << ",\"new_id\":" << frame.new_id;
        break;
    case MapFrameType::TracksRemove:
        out << "\"type\":\"tracks_remove\",\"timestamp\":" << frame.timestamp.count() << ",\"ids\":[";
        for (std::size_t i = 0; i < frame.removed_ids.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << frame.removed_ids[i];
        }
        out << ']';
        break;
    }

    out << '}';
    return out.str();
}

} // namespace tmcmt::map
