#include "trafficmcmt/map/MapIdRegistry.h"

#include <algorithm>
#include <functional>

#include "trafficmcmt/map/MapTypes.h"

namespace tmcmt::map {

std::size_t TrackKeyHash::operator()(const TrackKey& key) const noexcept {
    std::size_t seed = std::hash<std::string>{}(key.session_id);
    seed ^= std::hash<std::string>{}(key.camera) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<std::uint64_t>{}(key.local_track_id) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    return seed;
}

MapIdRegistry::MapIdRegistry(std::uint64_t first_id) : m_next(first_id == 0 ? 1 : first_id) {}

std::uint64_t MapIdRegistry::Assign(const TrackKey& key) {
    const auto existing = m_ids.find(key);
    if (existing != m_ids.end()) {
        return existing->second;
    }
    const std::uint64_t id = m_next;
    m_ids.emplace(key, id);
    ++m_next;
    return id;
}

std::optional<std::uint64_t> MapIdRegistry::Find(const TrackKey& key) const {
    const auto it = m_ids.find(key);
    if (it == m_ids.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool MapIdRegistry::Rename(std::uint64_t old_id, std::uint64_t new_id) {
    if (old_id == new_id || new_id == 0 || new_id > kMaxJsSafeId) {
        return false;
    }
    for (auto& [key, id] : m_ids) {
        (void)key;
        if (id == new_id) {
            return false;
        }
    }
    for (auto& [key, id] : m_ids) {
        (void)key;
        if (id == old_id) {
            id = new_id;
            m_next = std::max(m_next, new_id + 1);
            return true;
        }
    }
    return false;
}

} // namespace tmcmt::map
