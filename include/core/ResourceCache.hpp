#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace OpenArtOfWar
{

struct ResourceCacheStats
{
    std::size_t entryCount {0};
    std::size_t pinnedCount {0};
    std::size_t totalBytes {0};
    std::uint64_t hits {0};
    std::uint64_t misses {0};
    std::uint64_t evictions {0};
};

/**
 * @brief Generic LRU + TTL resource cache with reference pinning.
 *
 * Entries with `pinCount > 0` are protected from automatic eviction.
 */
template <typename T, typename Deleter = std::default_delete<T>, typename Key = std::string>
class ResourceCache
{
public:
    using KeyType = Key;
    using Ptr = std::unique_ptr<T, Deleter>;
    using Compare = std::conditional_t<std::is_same_v<Key, std::string>, std::less<>, std::less<Key>>;
    using LookupKey = std::conditional_t<std::is_same_v<Key, std::string>, std::string_view, const Key &>;

    struct Entry
    {
        Ptr value;
        std::size_t bytes {0};
        std::uint64_t lastUsedMs {0};
        int pinCount {0};
    };

    ResourceCache() = default;
    ~ResourceCache()
    {
        Clear();
    }

    ResourceCache(const ResourceCache &) = delete;
    ResourceCache &operator=(const ResourceCache &) = delete;
    ResourceCache(ResourceCache &&) noexcept = default;
    ResourceCache &operator=(ResourceCache &&) noexcept = default;

    T *Find(LookupKey key, std::uint64_t nowMs)
    {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
        {
            ++m_misses;
            return nullptr;
        }
        ++m_hits;
        it->second.lastUsedMs = nowMs;
        return it->second.value.get();
    }

    bool Contains(LookupKey key) const
    {
        return m_entries.find(key) != m_entries.end();
    }

    template <typename Loader>
    T *GetOrLoad(LookupKey key, Loader &&loader, std::uint64_t nowMs)
    {
        if (T *existing = Find(key, nowMs))
            return existing;

        std::size_t bytes = 0;
        Ptr loaded = loader(bytes);
        if (!loaded)
            return nullptr;

        T *raw = loaded.get();
        Insert(key, std::move(loaded), bytes, nowMs);
        return raw;
    }

    T *Insert(LookupKey key, Ptr value, std::size_t bytes, std::uint64_t nowMs)
    {
        if (!value)
            return nullptr;
        auto it = m_entries.find(key);
        if (it != m_entries.end())
        {
            m_totalBytes -= it->second.bytes;
            it->second.value = std::move(value);
            it->second.bytes = bytes;
            it->second.lastUsedMs = nowMs;
            m_totalBytes += bytes;
            return it->second.value.get();
        }
        Entry entry;
        entry.value = std::move(value);
        entry.bytes = bytes;
        entry.lastUsedMs = nowMs;
        T *raw = entry.value.get();
        m_totalBytes += bytes;
        m_entries.emplace(Key(key), std::move(entry));
        return raw;
    }

    bool Touch(LookupKey key, std::uint64_t nowMs)
    {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            return false;
        it->second.lastUsedMs = nowMs;
        return true;
    }

    bool Pin(LookupKey key)
    {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            return false;
        ++it->second.pinCount;
        return true;
    }

    bool Unpin(LookupKey key)
    {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            return false;
        if (it->second.pinCount > 0)
            --it->second.pinCount;
        return true;
    }

    bool Remove(LookupKey key)
    {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            return false;
        m_totalBytes -= it->second.bytes;
        m_entries.erase(it);
        return true;
    }

    std::size_t EvictUnused(std::uint64_t nowMs, std::uint64_t maxIdleMs, std::size_t maxBudgetBytes = 0)
    {
        std::size_t removed = 0;

        // 1. TTL eviction for unpinned entries
        for (auto it = m_entries.begin(); it != m_entries.end();)
        {
            const Entry &e = it->second;
            const bool expired = (maxIdleMs == 0) || (nowMs >= e.lastUsedMs && (nowMs - e.lastUsedMs) >= maxIdleMs);
            if (e.pinCount <= 0 && expired)
            {
                m_totalBytes -= e.bytes;
                it = m_entries.erase(it);
                ++removed;
                ++m_evictions;
            }
            else
            {
                ++it;
            }
        }

        // 2. LRU eviction if over byte budget
        if (maxBudgetBytes > 0 && m_totalBytes > maxBudgetBytes)
        {
            using Iter = typename std::map<Key, Entry, Compare>::iterator;
            std::vector<Iter> candidates;
            candidates.reserve(m_entries.size());
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            {
                if (it->second.pinCount <= 0)
                    candidates.push_back(it);
            }
            std::sort(candidates.begin(), candidates.end(), [](Iter a, Iter b) {
                return a->second.lastUsedMs < b->second.lastUsedMs;
            });
            for (Iter it : candidates)
            {
                if (m_totalBytes <= maxBudgetBytes)
                    break;
                m_totalBytes -= it->second.bytes;
                m_entries.erase(it);
                ++removed;
                ++m_evictions;
            }
        }

        return removed;
    }

    template <typename Fn>
    void ForEach(Fn &&fn)
    {
        for (auto &[key, entry] : m_entries)
            fn(key, entry.value.get());
    }

    template <typename Fn>
    void ForEach(Fn &&fn) const
    {
        for (const auto &[key, entry] : m_entries)
            fn(key, entry.value.get());
    }

    void Clear()
    {
        m_entries.clear();
        m_totalBytes = 0;
    }

    ResourceCacheStats Stats() const
    {
        ResourceCacheStats s;
        s.entryCount = m_entries.size();
        s.totalBytes = m_totalBytes;
        s.hits = m_hits;
        s.misses = m_misses;
        s.evictions = m_evictions;
        for (const auto &[key, entry] : m_entries)
        {
            if (entry.pinCount > 0)
                ++s.pinnedCount;
        }
        return s;
    }

    std::size_t Size() const
    {
        return m_entries.size();
    }

private:
    std::map<Key, Entry, Compare> m_entries;
    std::size_t m_totalBytes {0};
    std::uint64_t m_hits {0};
    std::uint64_t m_misses {0};
    std::uint64_t m_evictions {0};
};

} // namespace OpenArtOfWar
