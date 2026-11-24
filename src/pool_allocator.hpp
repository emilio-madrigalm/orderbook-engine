#pragma once

#include <cstddef>
#include <new>
#include <vector>

namespace ob {

// Free-list arena owned by one OrderBook. Blocks are carved from 64 KiB chunks
// and recycled through per-size free lists in 16-byte steps up to 256 bytes.
// Larger requests go straight to the global heap. Chunks are released when the
// arena is destroyed, so memory is returned to the arena, not to the heap, while
// the book lives.
class NodeArena {
public:
    NodeArena() = default;
    NodeArena(const NodeArena&)            = delete;
    NodeArena& operator=(const NodeArena&) = delete;
    ~NodeArena() {
        for (void* chunk : chunks_) ::operator delete(chunk);
    }

    void* allocate(std::size_t bytes) {
        if (bytes > kMaxSmall) return ::operator new(bytes);
        const std::size_t cls = size_class(bytes);
        if (void* block = free_[cls]) {
            free_[cls] = *static_cast<void**>(block);
            return block;
        }
        return carve(cls);
    }

    void deallocate(void* block, std::size_t bytes) noexcept {
        if (bytes > kMaxSmall) {
            ::operator delete(block);
            return;
        }
        const std::size_t cls = size_class(bytes);
        *static_cast<void**>(block) = free_[cls];
        free_[cls]                  = block;
    }

private:
    static constexpr std::size_t kStep     = 16;
    static constexpr std::size_t kMaxSmall = 256;
    static constexpr std::size_t kChunk    = 64 * 1024;

    static std::size_t size_class(std::size_t bytes) { return (bytes + kStep - 1) / kStep; }

    void* carve(std::size_t cls) {
        const std::size_t size = cls * kStep;
        if (static_cast<std::size_t>(end_ - cursor_) < size) {
            char* chunk = static_cast<char*>(::operator new(kChunk));
            chunks_.push_back(chunk);
            cursor_ = chunk;
            end_    = chunk + kChunk;
        }
        void* block = cursor_;
        cursor_ += size;
        return block;
    }

    void*             free_[kMaxSmall / kStep + 1]{};
    char*             cursor_{nullptr};
    char*             end_{nullptr};
    std::vector<void*> chunks_;
};

// Standard allocator backed by a NodeArena. Not owning: the arena must outlive
// every container that uses it. Rebinding to another type keeps the same arena.
template <class T>
class PoolAllocator {
public:
    using value_type = T;

    explicit PoolAllocator(NodeArena* arena) noexcept : arena_(arena) {}

    template <class U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept : arena_(other.arena()) {}

    T* allocate(std::size_t n) {
        return static_cast<T*>(arena_->allocate(n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept { arena_->deallocate(p, n * sizeof(T)); }

    NodeArena* arena() const noexcept { return arena_; }

    template <class U>
    bool operator==(const PoolAllocator<U>& other) const noexcept {
        return arena_ == other.arena();
    }
    template <class U>
    bool operator!=(const PoolAllocator<U>& other) const noexcept {
        return arena_ != other.arena();
    }

private:
    NodeArena* arena_;
};

}  // namespace ob
