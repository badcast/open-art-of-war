#pragma once

#include "core/ResourceCache.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenArtOfWar
{

/**
 * @brief Central coordinator for lazy-loaded resource caches and automatic LRU/TTL eviction.
 */
class ResourceManager
{
public:
    struct CachePolicy
    {
        std::uint64_t maxIdleMs {60000};                // 60s default idle TTL
        std::size_t maxBudgetBytes {256 * 1024 * 1024}; // 256 MB soft budget
    };

    struct NamedStats
    {
        std::string name;
        ResourceCacheStats stats;
    };

    using EvictCallback = std::function<std::size_t(std::uint64_t nowMs, bool forceAllUnpinned)>;
    using StatsCallback = std::function<ResourceCacheStats()>;

    ResourceManager() = default;
    ~ResourceManager() = default;

    void RegisterCache(std::string name, EvictCallback evictFn, StatsCallback statsFn);
    void UnregisterCache(std::string_view name);

    // Call once per frame; throttled internally (every kTickIntervalMs) so overhead is negligible.
    std::size_t Tick(std::uint64_t nowMs);

    // Force immediate eviction of all unpinned entries (e.g. when returning from match to menu).
    std::size_t UnloadUnusedNow(std::uint64_t nowMs);

    std::vector<NamedStats> GetAllStats() const;
    ResourceCacheStats GetTotalStats() const;

    void SetDefaultPolicy(const CachePolicy &policy)
    {
        m_policy = policy;
    }
    const CachePolicy &DefaultPolicy() const
    {
        return m_policy;
    }

private:
    struct Entry
    {
        std::string name;
        EvictCallback evictFn;
        StatsCallback statsFn;
    };

    static constexpr std::uint64_t kTickIntervalMs = 1000;
    CachePolicy m_policy;
    std::uint64_t m_lastTickMs {0};
    std::vector<Entry> m_caches;
};

} // namespace OpenArtOfWar
