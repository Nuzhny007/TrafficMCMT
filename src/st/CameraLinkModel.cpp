#include "trafficmcmt/st/CameraLinkModel.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <string>
#include <utility>

namespace tmcmt::st {
namespace {

Status ParseError(const std::string& where, const std::string& what) {
    return Status::Error(ErrorCode::ParseError, where + ": " + what);
}

std::string Join(const std::string& prefix, const std::string& key) {
    if (prefix.empty()) {
        return key;
    }
    return prefix + "." + key;
}

bool HasKey(const YAML::Node& node, const char* key) {
    return node && node.IsMap() && static_cast<bool>(node[key]);
}

Status ReadScalar(const YAML::Node& parent, const char* key, double& out, const std::string& where) {
    const YAML::Node node = parent[key];
    if (!node) {
        return Status::Ok();
    }
    try {
        out = node.as<double>();
    } catch (const YAML::Exception& error) {
        return ParseError(Join(where, key), error.what());
    }
    if (!std::isfinite(out)) {
        return ParseError(Join(where, key), "must be finite");
    }
    return Status::Ok();
}

Status ReadScalar(const YAML::Node& parent, const char* key, std::int64_t& out, const std::string& where) {
    const YAML::Node node = parent[key];
    if (!node) {
        return Status::Ok();
    }
    try {
        out = node.as<std::int64_t>();
    } catch (const YAML::Exception& error) {
        return ParseError(Join(where, key), error.what());
    }
    return Status::Ok();
}

Status ReadScalar(const YAML::Node& parent, const char* key, bool& out, const std::string& where) {
    const YAML::Node node = parent[key];
    if (!node) {
        return Status::Ok();
    }
    try {
        out = node.as<bool>();
    } catch (const YAML::Exception& error) {
        return ParseError(Join(where, key), error.what());
    }
    return Status::Ok();
}

Status ReadScalar(const YAML::Node& parent, const char* key, std::string& out, const std::string& where) {
    const YAML::Node node = parent[key];
    if (!node) {
        return Status::Ok();
    }
    try {
        out = node.as<std::string>();
    } catch (const YAML::Exception& error) {
        return ParseError(Join(where, key), error.what());
    }
    return Status::Ok();
}

bool IsFinitePolygon(const Polygon& polygon) {
    return std::all_of(polygon.points.begin(), polygon.points.end(),
                       [](const EnuPoint& point) { return point.IsFinite(); });
}

Status ParsePolygon(const YAML::Node& node, const std::string& where, Polygon& out) {
    out.points.clear();
    if (!node || node.IsNull()) {
        return Status::Ok();
    }
    if (!node.IsSequence()) {
        return ParseError(where, "expected a sequence of [east, north] pairs");
    }
    if (node.size() == 0) {
        return Status::Ok();
    }

    try {
        const bool nested = node[0].IsSequence();
        if (nested) {
            for (std::size_t i = 0; i < node.size(); ++i) {
                const YAML::Node pair = node[i];
                if (!pair.IsSequence() || pair.size() != 2) {
                    return ParseError(where, "point " + std::to_string(i) +
                                                 " must be a [east, north] pair");
                }
                out.points.push_back(EnuPoint{pair[0].as<double>(), pair[1].as<double>()});
            }
        } else {
            if (node.size() % 2 != 0) {
                return ParseError(where, "flat coordinate list must contain an even number of values");
            }
            for (std::size_t i = 0; i < node.size(); i += 2) {
                out.points.push_back(EnuPoint{node[i].as<double>(), node[i + 1].as<double>()});
            }
        }
    } catch (const YAML::Exception& error) {
        return ParseError(where, error.what());
    }

    if (out.points.size() < 3) {
        return ParseError(where, "a polygon needs at least 3 points");
    }
    if (!IsFinitePolygon(out)) {
        return ParseError(where, "coordinates must be finite");
    }
    return Status::Ok();
}

Status ValidateLink(const CameraLink& link) {
    if (link.from.empty() || link.to.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "camera link has an empty endpoint");
    }
    if (link.from == link.to) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "camera link " + link.from + " -> " + link.to + " is a self-link");
    }
    if (link.transition && !link.transition->IsValid()) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "camera link " + link.from + " -> " + link.to +
                                 " has invalid transition statistics");
    }
    return Status::Ok();
}

Status ValidateZones(const CameraZones& zones) {
    if (!zones.entry_zone.points.empty() &&
        (!zones.entry_zone.IsValid() || !IsFinitePolygon(zones.entry_zone))) {
        return Status::Error(ErrorCode::InvalidArgument, "invalid entry zone");
    }
    if (!zones.exit_zone.points.empty() &&
        (!zones.exit_zone.IsValid() || !IsFinitePolygon(zones.exit_zone))) {
        return Status::Error(ErrorCode::InvalidArgument, "invalid exit zone");
    }
    return Status::Ok();
}

Result<CameraLinkModel> ParseModel(const YAML::Node& root, const std::string& source) {
    CameraLinkModel model;
    if (!root || root.IsNull()) {
        return model;
    }
    if (!root.IsMap()) {
        return ParseError(source, "expected a mapping at the document root");
    }

    bool defaultLinked = false;
    if (Status status = ReadScalar(root, "default_linked", defaultLinked, source); !status.ok()) {
        return status;
    }
    model.SetDefaultLinked(defaultLinked);

    if (const YAML::Node fallback = root["fallback"]; fallback) {
        if (!fallback.IsMap()) {
            return ParseError(source + ".fallback", "expected a mapping");
        }
        TransitionWindow window = model.Fallback();
        if (Status status = ReadScalar(fallback, "min_transition_ms", window.min_ms,
                                       source + ".fallback");
            !status.ok()) {
            return status;
        }
        if (Status status = ReadScalar(fallback, "max_transition_ms", window.max_ms,
                                       source + ".fallback");
            !status.ok()) {
            return status;
        }
        if (!window.IsValid()) {
            return ParseError(source + ".fallback",
                              "requires 0 <= min_transition_ms < max_transition_ms");
        }
        model.SetFallback(window);
    }

    if (const YAML::Node cameras = root["cameras"]; cameras) {
        if (!cameras.IsMap()) {
            return ParseError(source + ".cameras", "expected a mapping");
        }
        for (const auto& item : cameras) {
            CameraId camera;
            try {
                camera = item.first.as<std::string>();
            } catch (const YAML::Exception& error) {
                return ParseError(source + ".cameras", error.what());
            }
            if (camera.empty()) {
                return ParseError(source + ".cameras", "camera id must not be empty");
            }
            const std::string where = source + ".cameras." + camera;
            const YAML::Node node = item.second;
            if (!node.IsMap()) {
                return ParseError(where, "expected a mapping");
            }

            CameraZones zones;
            if (const YAML::Node entry = node["entry_zone"]; entry) {
                if (Status status = ParsePolygon(entry, where + ".entry_zone", zones.entry_zone);
                    !status.ok()) {
                    return status;
                }
            }
            if (const YAML::Node exit = node["exit_zone"]; exit) {
                if (Status status = ParsePolygon(exit, where + ".exit_zone", zones.exit_zone);
                    !status.ok()) {
                    return status;
                }
            }
            if (Status status = model.SetZones(camera, zones); !status.ok()) {
                return ParseError(where, status.message());
            }
        }
    }

    if (const YAML::Node links = root["links"]; links) {
        if (!links.IsSequence()) {
            return ParseError(source + ".links", "expected a sequence");
        }
        std::set<std::pair<std::string, std::string>> seen;
        for (std::size_t i = 0; i < links.size(); ++i) {
            const YAML::Node node = links[i];
            const std::string where = source + ".links[" + std::to_string(i) + "]";
            if (!node.IsMap()) {
                return ParseError(where, "expected a mapping");
            }

            CameraLink link;
            if (Status status = ReadScalar(node, "from", link.from, where); !status.ok()) {
                return status;
            }
            if (Status status = ReadScalar(node, "to", link.to, where); !status.ok()) {
                return status;
            }
            if (Status status = ReadScalar(node, "enabled", link.enabled, where); !status.ok()) {
                return status;
            }

            const bool hasMean = HasKey(node, "mean_ms");
            const bool hasStddev = HasKey(node, "stddev_ms");
            const bool hasSamples = HasKey(node, "samples");
            if (hasMean || hasStddev || hasSamples) {
                if (!(hasMean && hasStddev && hasSamples)) {
                    return ParseError(where,
                                      "mean_ms, stddev_ms and samples must be provided together");
                }
                TransitionStats stats;
                if (Status status = ReadScalar(node, "mean_ms", stats.mean_ms, where); !status.ok()) {
                    return status;
                }
                if (Status status = ReadScalar(node, "stddev_ms", stats.stddev_ms, where);
                    !status.ok()) {
                    return status;
                }
                if (Status status = ReadScalar(node, "samples", stats.samples, where); !status.ok()) {
                    return status;
                }
                if (!stats.IsValid()) {
                    return ParseError(where,
                                      "requires a finite mean_ms, stddev_ms > 0 and samples >= 0");
                }
                link.transition = stats;
            }

            if (Status status = ValidateLink(link); !status.ok()) {
                return ParseError(where, status.message());
            }
            if (!seen.emplace(link.from, link.to).second) {
                return ParseError(where, "duplicate link " + link.from + " -> " + link.to);
            }
            if (Status status = model.SetLink(link); !status.ok()) {
                return ParseError(where, status.message());
            }
        }
    }

    return model;
}

void WritePolygon(YAML::Node& parent, const char* key, const Polygon& polygon) {
    if (polygon.points.empty()) {
        return;
    }
    YAML::Node sequence(YAML::NodeType::Sequence);
    for (const EnuPoint& point : polygon.points) {
        YAML::Node pair(YAML::NodeType::Sequence);
        pair.push_back(point.east);
        pair.push_back(point.north);
        sequence.push_back(pair);
    }
    parent[key] = sequence;
}

} // namespace

Result<CameraLinkModel> CameraLinkModel::Load(const std::string& path) {
    try {
        return ParseModel(YAML::LoadFile(path), path);
    } catch (const YAML::Exception& error) {
        return Status::Error(ErrorCode::IoError, "cannot load camera links '" + path + "': " + error.what());
    } catch (const std::exception& error) {
        return Status::Error(ErrorCode::IoError, "cannot load camera links '" + path + "': " + error.what());
    }
}

Result<CameraLinkModel> CameraLinkModel::LoadFromString(const std::string& text, const std::string& source) {
    try {
        return ParseModel(YAML::Load(text), source);
    } catch (const YAML::Exception& error) {
        return Status::Error(ErrorCode::ParseError, source + ": " + error.what());
    }
}

Status CameraLinkModel::Save(const std::string& path) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Status::Error(ErrorCode::IoError, "cannot open camera links '" + path + "' for writing");
    }
    output << "# TrafficMCMT camera link model.\n"
              "# Zones are ENU metres in the shared origin of the tracklet stream.\n"
           << Dump();
    if (!output.good()) {
        return Status::Error(ErrorCode::IoError, "failed to write camera links '" + path + "'");
    }
    return Status::Ok();
}

std::string CameraLinkModel::Dump() const {
    YAML::Node root;
    root["default_linked"] = m_defaultLinked;
    root["fallback"]["min_transition_ms"] = m_fallback.min_ms;
    root["fallback"]["max_transition_ms"] = m_fallback.max_ms;

    bool hasZones = false;
    for (const auto& [camera, zones] : m_zones) {
        if (zones.entry_zone.points.empty() && zones.exit_zone.points.empty()) {
            continue;
        }
        hasZones = true;
        YAML::Node node;
        WritePolygon(node, "entry_zone", zones.entry_zone);
        WritePolygon(node, "exit_zone", zones.exit_zone);
        root["cameras"][camera] = node;
    }
    (void)hasZones;

    if (!m_links.empty()) {
        YAML::Node links(YAML::NodeType::Sequence);
        for (const auto& [key, link] : m_links) {
            (void)key;
            YAML::Node node;
            node["from"] = link.from;
            node["to"] = link.to;
            node["enabled"] = link.enabled;
            if (link.transition) {
                node["mean_ms"] = link.transition->mean_ms;
                node["stddev_ms"] = link.transition->stddev_ms;
                node["samples"] = link.transition->samples;
            }
            links.push_back(node);
        }
        root["links"] = links;
    }

    return YAML::Dump(root) + "\n";
}

bool CameraLinkModel::IsLinked(const CameraId& from, const CameraId& to) const {
    if (from.empty() || to.empty() || from == to) {
        return false;
    }
    const auto it = m_links.find(LinkKey{from, to});
    if (it != m_links.end()) {
        return it->second.enabled;
    }
    return m_defaultLinked;
}

std::optional<TransitionStats> CameraLinkModel::Transition(const CameraId& from, const CameraId& to) const {
    const auto it = m_links.find(LinkKey{from, to});
    if (it == m_links.end() || !it->second.enabled) {
        return std::nullopt;
    }
    return it->second.transition;
}

const CameraZones* CameraLinkModel::Zones(const CameraId& camera) const {
    const auto it = m_zones.find(camera);
    return it == m_zones.end() ? nullptr : &it->second;
}

Status CameraLinkModel::SetLink(const CameraLink& link) {
    if (Status status = ValidateLink(link); !status.ok()) {
        return status;
    }
    m_links[LinkKey{link.from, link.to}] = link;
    return Status::Ok();
}

Status CameraLinkModel::SetZones(const CameraId& camera, const CameraZones& zones) {
    if (camera.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "camera id must not be empty");
    }
    if (Status status = ValidateZones(zones); !status.ok()) {
        return status;
    }
    m_zones[camera] = zones;
    return Status::Ok();
}

std::vector<CameraLink> CameraLinkModel::Links() const {
    std::vector<CameraLink> links;
    links.reserve(m_links.size());
    for (const auto& [key, link] : m_links) {
        (void)key;
        links.push_back(link);
    }
    return links;
}

std::vector<CameraId> CameraLinkModel::Cameras() const {
    std::set<CameraId> cameras;
    for (const auto& [camera, zones] : m_zones) {
        (void)zones;
        cameras.insert(camera);
    }
    for (const auto& [key, link] : m_links) {
        (void)key;
        cameras.insert(link.from);
        cameras.insert(link.to);
    }
    return std::vector<CameraId>(cameras.begin(), cameras.end());
}

} // namespace tmcmt::st
