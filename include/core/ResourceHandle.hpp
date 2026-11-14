#pragma once

#include "core/ResourceCache.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace OpenArtOfWar
{

/**
 * @brief RAII lease that keeps a cached resource pinned until the lease is destroyed.
 */
template <typename CacheT>
class ResourceLease
{
public:
    using KeyType = typename CacheT::KeyType;

    ResourceLease() = default;
    ResourceLease(CacheT *cache, KeyType key)
        : m_cache(cache)
        , m_key(std::move(key))
    {
        if (m_cache && m_cache->Pin(m_key))
            m_pinned = true;
    }

    ~ResourceLease()
    {
        Release();
    }

    ResourceLease(const ResourceLease &) = delete;
    ResourceLease &operator=(const ResourceLease &) = delete;

    ResourceLease(ResourceLease &&other) noexcept
        : m_cache(other.m_cache)
        , m_key(std::move(other.m_key))
        , m_pinned(other.m_pinned)
    {
        other.m_cache = nullptr;
        other.m_pinned = false;
    }

    ResourceLease &operator=(ResourceLease &&other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_cache = other.m_cache;
            m_key = std::move(other.m_key);
            m_pinned = other.m_pinned;
            other.m_cache = nullptr;
            other.m_pinned = false;
        }
        return *this;
    }

    void Release()
    {
        if (m_cache && m_pinned)
            m_cache->Unpin(m_key);
        m_cache = nullptr;
        m_pinned = false;
    }

    bool IsPinned() const
    {
        return m_pinned;
    }

private:
    CacheT *m_cache {nullptr};
    KeyType m_key {};
    bool m_pinned {false};
};

} // namespace OpenArtOfWar
