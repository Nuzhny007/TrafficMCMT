#include "trafficmcmt/core/TrackletId.h"

#include <cstddef>

namespace tmcmt {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::uint64_t Fnv1a(std::uint64_t hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

template <typename T>
std::uint64_t HashValue(std::uint64_t hash, const T& value) noexcept {
    return Fnv1a(hash, &value, sizeof(T));
}

} // namespace

std::size_t TrackletKeyHash::operator()(const TrackletKey& key) const noexcept {
    std::uint64_t hash = kFnvOffset;
    hash = Fnv1a(hash, key.session_id.data(), key.session_id.size());
    hash = HashValue(hash, key.camera.size());
    hash = Fnv1a(hash, key.camera.data(), key.camera.size());
    hash = HashValue(hash, key.local_track_id);
    hash = HashValue(hash, key.sequence);
    return static_cast<std::size_t>(hash);
}

TrackletKey MakeTrackletKey(const Tracklet& tracklet) {
    return TrackletKey{tracklet.session_id, tracklet.camera, tracklet.local_track_id, tracklet.sequence};
}

TrackletId MakeTrackletId(std::string_view session_id, std::string_view camera, std::uint64_t local_track_id,
                          std::uint64_t sequence) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash = HashValue(hash, session_id.size());
    hash = Fnv1a(hash, session_id.data(), session_id.size());
    hash = HashValue(hash, camera.size());
    hash = Fnv1a(hash, camera.data(), camera.size());
    hash = HashValue(hash, local_track_id);
    hash = HashValue(hash, sequence);

    // 0 is reserved for "invalid id".
    return hash == 0 ? 1 : static_cast<TrackletId>(hash);
}

void AssignTrackletId(Tracklet& tracklet) noexcept {
    tracklet.id = MakeTrackletId(tracklet.session_id, tracklet.camera, tracklet.local_track_id, tracklet.sequence);
}

} // namespace tmcmt
