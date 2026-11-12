#include "trafficmcmt/bev/WorldTrajectoryFusion.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <utility>
#include <vector>

#include "trafficmcmt/geo/VelocityEstimator.h"

namespace tmcmt::bev {
namespace {

constexpr double kRadiansToDegrees = 57.295779513082320876;
constexpr double kMaxSpeedMps = 120.0;

bool HasOrigin(const GeoPoint& point) noexcept {
    return point.IsFinite() && (point.latitude != 0.0 || point.longitude != 0.0);
}

bool SameOrigin(const GeoPoint& lhs, const GeoPoint& rhs) noexcept {
    return lhs.latitude == rhs.latitude && lhs.longitude == rhs.longitude;
}

EnuPoint Delta(const GeoPoint& from, const GeoPoint& to) {
    if (!HasOrigin(from) || !HasOrigin(to) || SameOrigin(from, to)) {
        return EnuPoint{};
    }
    return GeographicToEnu(from, to);
}

void Translate(WorldPoint& point, const EnuPoint& delta) {
    point.x = static_cast<float>(static_cast<double>(point.x) + delta.east);
    point.y = static_cast<float>(static_cast<double>(point.y) + delta.north);
}

bool BetterSample(const WorldSample& candidate, const WorldSample& existing) noexcept {
    if (candidate.quality != existing.quality) {
        return candidate.quality > existing.quality;
    }
    if (candidate.tracklet_id != existing.tracklet_id) {
        return candidate.tracklet_id < existing.tracklet_id;
    }
    if (candidate.camera != existing.camera) {
        return candidate.camera < existing.camera;
    }
    return candidate.ts < existing.ts;
}

void SmoothPositions(std::vector<WorldSample>& samples, int window) {
    if (window < 3 || samples.size() < 3) {
        return;
    }
    const std::size_t half = static_cast<std::size_t>(window / 2);
    std::vector<EnuPoint> original;
    original.reserve(samples.size());
    for (const WorldSample& sample : samples) {
        original.push_back(sample.position);
    }

    for (std::size_t i = 1; i + 1 < samples.size(); ++i) {
        const std::size_t begin = i > half ? i - half : 0;
        const std::size_t end = std::min(samples.size() - 1, i + half);
        EnuPoint sum{};
        std::size_t count = 0;
        for (std::size_t j = begin; j <= end; ++j) {
            sum.east += original[j].east;
            sum.north += original[j].north;
            ++count;
        }
        if (count > 0) {
            samples[i].position.east = sum.east / static_cast<double>(count);
            samples[i].position.north = sum.north / static_cast<double>(count);
        }
    }
}

} // namespace

WorldTrajectoryFusion::WorldTrajectoryFusion(WorldFusionOptions options) : m_options(options) {}

GeoPoint WorldTrajectoryFusion::ResolveTargetOrigin(const Tracklet& tracklet,
                                                    const CameraCalibration* calibration,
                                                    const GeoPoint& fallback) const {
    if (m_options.has_shared_origin && HasOrigin(m_options.shared_origin)) {
        return m_options.shared_origin;
    }
    if (tracklet.HasOrigin()) {
        return GeoPoint{tracklet.origin_latitude, tracklet.origin_longitude};
    }
    if (calibration != nullptr && calibration->HasOrigin()) {
        return GeoPoint{calibration->origin_latitude, calibration->origin_longitude};
    }
    if (HasOrigin(fallback)) {
        return fallback;
    }
    return GeoPoint{};
}

GeoPoint WorldTrajectoryFusion::ResolveSourceOrigin(const Tracklet& tracklet,
                                                    const CameraCalibration* calibration,
                                                    const GeoPoint& fallback) const {
    if (tracklet.HasOrigin()) {
        return GeoPoint{tracklet.origin_latitude, tracklet.origin_longitude};
    }
    if (calibration != nullptr && calibration->HasOrigin()) {
        return GeoPoint{calibration->origin_latitude, calibration->origin_longitude};
    }
    return fallback;
}

const HomographyProjector* WorldTrajectoryFusion::ProjectorFor(const CameraCalibration& calibration) {
    auto& cache = m_projectors[calibration.camera_id];
    const bool reusable =
        cache.calibration != nullptr && cache.calibration->version == calibration.version &&
        cache.calibration->homography == calibration.homography &&
        cache.calibration->homography_normalized == calibration.homography_normalized &&
        cache.calibration->image_width == calibration.image_width &&
        cache.calibration->image_height == calibration.image_height;
    if (reusable) {
        return cache.projector ? &*cache.projector : nullptr;
    }

    cache.calibration = std::make_shared<const CameraCalibration>(calibration);
    cache.projector.reset();
    auto projector = HomographyProjector::Create(calibration);
    if (projector) {
        cache.projector = std::move(projector).value();
    }
    return cache.projector ? &*cache.projector : nullptr;
}

Status WorldTrajectoryFusion::Normalize(Tracklet& tracklet, const CameraCalibration* calibration,
                                        const GeoPoint& fallback_origin) {
    const GeoPoint target = ResolveTargetOrigin(tracklet, calibration, fallback_origin);
    const bool hasTarget = HasOrigin(target);
    const GeoPoint source = ResolveSourceOrigin(tracklet, calibration, fallback_origin);
    const EnuPoint existingDelta = Delta(source, target);
    const EnuPoint calibrationDelta =
        calibration != nullptr && calibration->HasOrigin()
            ? Delta(GeoPoint{calibration->origin_latitude, calibration->origin_longitude}, target)
            : EnuPoint{};

    const HomographyProjector* projector = nullptr;
    if (calibration != nullptr && calibration->has_homography) {
        projector = ProjectorFor(*calibration);
    }

    for (Observation& observation : tracklet.observations) {
        ++m_stats.observations;
        const bool hasWorld = observation.world.IsFinite();

        if (hasWorld && !m_options.overwrite_world) {
            Translate(observation.world, existingDelta);
            continue;
        }

        if (projector != nullptr) {
            const WorldPoint projected =
                projector->ProjectBBox(observation.bbox, ImageSize{calibration->image_width,
                                                                   calibration->image_height});
            if (projected.IsFinite()) {
                observation.world = projected;
                Translate(observation.world, calibrationDelta);
                ++m_stats.projected;
                continue;
            }
            ++m_stats.projection_failures;
        }

        if (!hasWorld) {
            ++m_stats.missing_world;
        }
    }

    if (hasTarget) {
        tracklet.origin_latitude = target.latitude;
        tracklet.origin_longitude = target.longitude;
    }
    return Status::Ok();
}

WorldTrajectory WorldTrajectoryFusion::Fuse(const GlobalTrackUpdate& update) const {
    WorldTrajectory out;
    out.global_id = update.global_id;
    out.type = update.type;
    out.is_final = update.is_final;

    std::vector<WorldSample> raw;
    Timestamp begin = update.updated_at;
    Timestamp end = update.updated_at;
    bool haveTime = false;
    for (const Tracklet& segment : update.segments) {
        if (!haveTime) {
            begin = segment.begin;
            end = segment.end;
            haveTime = true;
        } else {
            begin = std::min(begin, segment.begin);
            end = std::max(end, segment.end);
        }
        if (!out.has_origin && segment.HasOrigin()) {
            out.origin = GeoPoint{segment.origin_latitude, segment.origin_longitude};
            out.has_origin = true;
        }
        if (out.type == ObjectType::Unknown && segment.type != ObjectType::Unknown) {
            out.type = segment.type;
        }
        for (const Observation& observation : segment.observations) {
            if (!observation.world.IsFinite()) {
                continue;
            }
            raw.push_back(WorldSample{observation.ts,
                                      EnuPoint{observation.world.x, observation.world.y},
                                      segment.camera,
                                      segment.id,
                                      segment.type,
                                      segment.quality});
        }
    }

    if (!out.has_origin && m_options.has_shared_origin && HasOrigin(m_options.shared_origin)) {
        out.origin = m_options.shared_origin;
        out.has_origin = true;
    }

    std::stable_sort(raw.begin(), raw.end(), [](const WorldSample& lhs, const WorldSample& rhs) {
        if (lhs.ts != rhs.ts) {
            return lhs.ts < rhs.ts;
        }
        if (lhs.camera != rhs.camera) {
            return lhs.camera < rhs.camera;
        }
        return lhs.tracklet_id < rhs.tracklet_id;
    });

    m_stats.samples_in += raw.size();

    const std::int64_t windowMs = std::max<std::int64_t>(0, m_options.duplicate_window_ms);
    const double distanceLimit = m_options.duplicate_distance_m;
    std::vector<WorldSample> kept;
    kept.reserve(raw.size());
    std::deque<std::size_t> recent;

    for (const WorldSample& candidate : raw) {
        while (!recent.empty() && candidate.ts - kept[recent.front()].ts > Timestamp(windowMs)) {
            recent.pop_front();
        }

        std::optional<std::size_t> match;
        if (distanceLimit > 0.0) {
            for (const std::size_t index : recent) {
                const WorldSample& existing = kept[index];
                if (std::llabs((candidate.ts - existing.ts).count()) > windowMs) {
                    continue;
                }
                if (existing.camera == candidate.camera) {
                    continue;
                }
                if (candidate.type != ObjectType::Unknown && existing.type != ObjectType::Unknown &&
                    candidate.type != existing.type) {
                    continue;
                }
                const double distance = std::hypot(candidate.position.east - existing.position.east,
                                                   candidate.position.north - existing.position.north);
                if (distance <= distanceLimit) {
                    match = index;
                    break;
                }
            }
        }

        if (match) {
            ++m_stats.overlap_pairs;
            ++m_stats.duplicates_suppressed;
            ++out.duplicates_suppressed;
            if (BetterSample(candidate, kept[*match])) {
                kept[*match] = candidate;
            }
            continue;
        }

        kept.push_back(candidate);
        recent.push_back(kept.size() - 1);
    }

    std::stable_sort(kept.begin(), kept.end(), [](const WorldSample& lhs, const WorldSample& rhs) {
        if (lhs.ts != rhs.ts) {
            return lhs.ts < rhs.ts;
        }
        if (lhs.camera != rhs.camera) {
            return lhs.camera < rhs.camera;
        }
        return lhs.tracklet_id < rhs.tracklet_id;
    });

    SmoothPositions(kept, m_options.smooth_window);

    if (!kept.empty()) {
        out.begin = kept.front().ts;
        out.end = kept.back().ts;

        geo::VelocityEstimator estimator(static_cast<double>(std::max(1, m_options.speed_window_ms)) / 1000.0,
                                         kMaxSpeedMps);
        for (const WorldSample& sample : kept) {
            estimator.Add(sample.ts, sample.position);
        }
        if (const std::optional<EnuPoint> velocity = estimator.Estimate(kept.back().ts)) {
            out.velocity = *velocity;
            out.has_velocity = true;
            out.speed_mps = std::hypot(velocity->east, velocity->north);
            double course = std::atan2(velocity->east, velocity->north) * kRadiansToDegrees;
            if (course < 0.0) {
                course += 360.0;
            }
            out.course_deg = course;
        }
    } else {
        out.begin = begin;
        out.end = end;
    }

    m_stats.samples_out += kept.size();
    out.samples = std::move(kept);
    return out;
}

FusionStats WorldTrajectoryFusion::Stats() const noexcept {
    return m_stats;
}

void WorldTrajectoryFusion::ResetStats() noexcept {
    m_stats = FusionStats{};
}

} // namespace tmcmt::bev
