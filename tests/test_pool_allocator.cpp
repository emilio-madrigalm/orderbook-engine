#include <gtest/gtest.h>

#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "pool_allocator.hpp"

namespace {

using ob::NodeArena;
using ob::PoolAllocator;

TEST(NodeArena, FreedBlockIsReusedForSameSize) {
    NodeArena arena;
    void* a = arena.allocate(40);
    arena.deallocate(a, 40);
    void* b = arena.allocate(40);
    EXPECT_EQ(a, b);
}

TEST(NodeArena, SizesRoundUpToSameClass) {
    NodeArena arena;
    void* a = arena.allocate(33);
    arena.deallocate(a, 33);
    void* b = arena.allocate(48);  // same 16-byte class as 33
    EXPECT_EQ(a, b);
}

TEST(NodeArena, BlocksAreSixteenByteAligned) {
    NodeArena arena;
    for (std::size_t bytes : {1u, 17u, 64u, 200u, 256u}) {
        void* p = arena.allocate(bytes);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16, 0u) << "bytes=" << bytes;
    }
}

TEST(NodeArena, LargeRequestsFallThroughToHeap) {
    NodeArena arena;
    void* big = arena.allocate(4096);
    ASSERT_NE(big, nullptr);
    arena.deallocate(big, 4096);  // must not corrupt the arena
    void* small = arena.allocate(16);
    EXPECT_NE(small, nullptr);
}

TEST(NodeArena, GrowsAcrossChunks) {
    NodeArena arena;
    std::vector<void*> blocks;
    for (int i = 0; i < 20000; ++i) blocks.push_back(arena.allocate(64));  // > one 64 KiB chunk
    for (void* p : blocks) arena.deallocate(p, 64);
    void* again = arena.allocate(64);
    EXPECT_NE(again, nullptr);
}

TEST(PoolAllocator, RebindKeepsSameArena) {
    NodeArena arena;
    PoolAllocator<int> a(&arena);
    PoolAllocator<double> b(a);
    EXPECT_EQ(a, PoolAllocator<char>(b));
}

TEST(PoolAllocator, ContainersWorkAndReuseNodes) {
    NodeArena arena;
    std::list<int, PoolAllocator<int>> l{PoolAllocator<int>(&arena)};
    for (int i = 0; i < 1000; ++i) l.push_back(i);
    l.clear();
    for (int i = 0; i < 1000; ++i) l.push_back(i);
    EXPECT_EQ(l.size(), 1000u);

    using Pair = std::pair<const int, int>;
    std::map<int, int, std::less<int>, PoolAllocator<Pair>> m(std::less<int>{},
                                                               PoolAllocator<Pair>(&arena));
    for (int i = 0; i < 500; ++i) m[i] = i * 2;
    for (int i = 0; i < 500; ++i) m.erase(i);
    EXPECT_TRUE(m.empty());
}

}  // namespace
