#include "core/ResourceCache.hpp"
#include "core/ResourceHandle.hpp"
#include "core/ResourceManager.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>

using namespace OpenArtOfWar;

namespace
{

struct DummyAsset
{
    int id {0};
    int *destroyedCounter {nullptr};
    DummyAsset(int idValue, int *counter)
        : id(idValue)
        , destroyedCounter(counter)
    {
    }
    DummyAsset(const DummyAsset &) = delete;
    DummyAsset &operator=(const DummyAsset &) = delete;
    DummyAsset(DummyAsset &&) = delete;
    DummyAsset &operator=(DummyAsset &&) = delete;
    ~DummyAsset()
    {
        if (destroyedCounter)
            ++(*destroyedCounter);
    }
};

void TestLazyLoadAndCacheHit()
{
    ResourceCache<DummyAsset> cache;
    int loadCount = 0;
    int destroyed = 0;

    auto loader = [&](std::size_t &bytes) {
        ++loadCount;
        bytes = 1024;
        return std::make_unique<DummyAsset>(42, &destroyed);
    };

    DummyAsset *a = cache.GetOrLoad("alpha", loader, 100);
    assert(a != nullptr && a->id == 42);
    assert(loadCount == 1);

    DummyAsset *b = cache.GetOrLoad("alpha", loader, 200);
    assert(b == a);
    assert(loadCount == 1);

    const ResourceCacheStats s = cache.Stats();
    assert(s.entryCount == 1);
    assert(s.totalBytes == 1024);
    assert(s.hits == 1);
    assert(s.misses == 1);
}

void TestTtlAndPinProtection()
{
    ResourceCache<DummyAsset> cache;
    int destroyed = 0;

    cache.Insert("pinned", std::make_unique<DummyAsset>(1, &destroyed), 500, 1000);
    cache.Insert("temp", std::make_unique<DummyAsset>(2, &destroyed), 500, 1000);

    ResourceLease<ResourceCache<DummyAsset>> lease(&cache, "pinned");
    assert(lease.IsPinned());

    // Before TTL (10s): nothing evicted
    assert(cache.EvictUnused(5000, 10000) == 0);
    assert(cache.Size() == 2);

    // After TTL (11s): only "temp" is evicted, "pinned" survives
    assert(cache.EvictUnused(11000, 10000) == 1);
    assert(cache.Size() == 1);
    assert(destroyed == 1);
    assert(cache.Contains("pinned"));
    assert(!cache.Contains("temp"));

    // Release lease and advance past TTL: now "pinned" is evicted
    lease.Release();
    assert(cache.EvictUnused(12000, 10000) == 1);
    assert(cache.Size() == 0);
    assert(destroyed == 2);
}

void TestLruBudgetEviction()
{
    ResourceCache<DummyAsset> cache;
    int destroyed = 0;

    cache.Insert("a", std::make_unique<DummyAsset>(1, &destroyed), 400, 100);
    cache.Insert("b", std::make_unique<DummyAsset>(2, &destroyed), 400, 200);
    cache.Insert("c", std::make_unique<DummyAsset>(3, &destroyed), 400, 300);

    // Touch "a" so "b" becomes the oldest
    assert(cache.Touch("a", 350));

    // Budget is 800 bytes -> must evict 1 entry ("b")
    const std::size_t evicted = cache.EvictUnused(400, 10000, 800);
    assert(evicted == 1);
    assert(!cache.Contains("b"));
    assert(cache.Contains("a"));
    assert(cache.Contains("c"));
    assert(cache.Stats().totalBytes == 800);
    assert(destroyed == 1);
}

void TestResourceManagerTickAndForceUnload()
{
    ResourceManager rm;
    ResourceCache<DummyAsset> cache;
    int destroyed = 0;

    rm.RegisterCache(
        "dummy",
        [&](std::uint64_t nowMs, bool forceAll) {
            return cache.EvictUnused(nowMs, forceAll ? 0 : 5000, 0);
        },
        [&]() {
            return cache.Stats();
        });

    cache.Insert("x", std::make_unique<DummyAsset>(10, &destroyed), 256, 1000);
    cache.Insert("y", std::make_unique<DummyAsset>(11, &destroyed), 256, 1000);
    assert(cache.Pin("x"));

    // Throttled tick at 1500ms runs initial check, nothing expired yet
    assert(rm.Tick(1500) == 0);
    // Throttled out (< 1000ms since 1500ms)
    assert(rm.Tick(2000) == 0);

    // Force unload ignores TTL and removes unpinned "y", keeping pinned "x"
    assert(rm.UnloadUnusedNow(2100) == 1);
    assert(cache.Contains("x"));
    assert(!cache.Contains("y"));
    assert(rm.GetTotalStats().entryCount == 1);
    assert(rm.GetTotalStats().pinnedCount == 1);
    assert(destroyed == 1);
}

} // namespace

int main()
{
    TestLazyLoadAndCacheHit();
    TestTtlAndPinProtection();
    TestLruBudgetEviction();
    TestResourceManagerTickAndForceUnload();
    std::cout << "[ResourceCacheTests] All tests passed." << std::endl;
    return 0;
}
