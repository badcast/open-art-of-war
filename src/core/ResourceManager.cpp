#include "core/ResourceManager.hpp"

#include <algorithm>

namespace OpenArtOfWar
{

void ResourceManager::RegisterCache(std::string name, EvictCallback evictFn, StatsCallback statsFn)
{
    UnregisterCache(name);
    m_caches.push_back(Entry {std::move(name), std::move(evictFn), std::move(statsFn)});
}

void ResourceManager::UnregisterCache(std::string_view name)
{
    m_caches.erase(
        std::remove_if(
            m_caches.begin(),
            m_caches.end(),
            [name](const Entry &e) {
                return e.name == name;
            }),
        m_caches.end());
}

std::size_t ResourceManager::Tick(std::uint64_t nowMs)
{
    if (m_lastTickMs != 0 && nowMs >= m_lastTickMs && (nowMs - m_lastTickMs) < kTickIntervalMs)
        return 0;
    m_lastTickMs = nowMs;

    std::size_t totalEvicted = 0;
    for (const Entry &e : m_caches)
    {
        if (e.evictFn)
            totalEvicted += e.evictFn(nowMs, false);
    }
    return totalEvicted;
}

std::size_t ResourceManager::UnloadUnusedNow(std::uint64_t nowMs)
{
    m_lastTickMs = nowMs;
    std::size_t totalEvicted = 0;
    for (const Entry &e : m_caches)
    {
        if (e.evictFn)
            totalEvicted += e.evictFn(nowMs, true);
    }
    return totalEvicted;
}

std::vector<ResourceManager::NamedStats> ResourceManager::GetAllStats() const
{
    std::vector<NamedStats> out;
    out.reserve(m_caches.size());
    for (const Entry &e : m_caches)
    {
        ResourceCacheStats s;
        if (e.statsFn)
            s = e.statsFn();
        out.push_back(NamedStats {e.name, s});
    }
    return out;
}

ResourceCacheStats ResourceManager::GetTotalStats() const
{
    ResourceCacheStats total;
    for (const Entry &e : m_caches)
    {
        if (!e.statsFn)
            continue;
        const ResourceCacheStats s = e.statsFn();
        total.entryCount += s.entryCount;
        total.pinnedCount += s.pinnedCount;
        total.totalBytes += s.totalBytes;
        total.hits += s.hits;
        total.misses += s.misses;
        total.evictions += s.evictions;
    }
    return total;
}

} // namespace OpenArtOfWar
