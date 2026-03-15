#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace lona {

class Arena {
    struct Block {
        std::unique_ptr<std::byte[]> storage;
        std::size_t size = 0;
        std::size_t used = 0;
    };

    struct DestructorRecord {
        void *ptr = nullptr;
        void (*destroy)(void *) = nullptr;
    };

    static constexpr std::size_t kDefaultBlockSize = 16 * 1024;

    std::size_t blockSize_ = kDefaultBlockSize;
    std::vector<Block> blocks_;
    std::vector<DestructorRecord> destructors_;

    static std::size_t alignUp(std::size_t value, std::size_t alignment) {
        const auto mask = alignment - 1;
        return (value + mask) & ~mask;
    }

    void allocateBlock(std::size_t minSize) {
        Block block;
        block.size = minSize > blockSize_ ? minSize : blockSize_;
        block.storage = std::make_unique<std::byte[]>(block.size);
        blocks_.push_back(std::move(block));
    }

    void *allocateRaw(std::size_t size, std::size_t alignment) {
        if (blocks_.empty()) {
            allocateBlock(size + alignment);
        }

        auto *block = &blocks_.back();
        auto offset = alignUp(block->used, alignment);
        if (offset + size > block->size) {
            allocateBlock(size + alignment);
            block = &blocks_.back();
            offset = alignUp(block->used, alignment);
        }

        void *ptr = block->storage.get() + offset;
        block->used = offset + size;
        return ptr;
    }

public:
    explicit Arena(std::size_t blockSize = kDefaultBlockSize)
        : blockSize_(blockSize) {}

    Arena(const Arena &) = delete;
    Arena &operator=(const Arena &) = delete;

    ~Arena() {
        for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it) {
            if (it->destroy) {
                it->destroy(it->ptr);
            }
        }
    }

    template<typename T, typename... Args>
    T *emplace(Args &&...args) {
        static_assert(!std::is_reference_v<T>);
        void *storage = allocateRaw(sizeof(T), alignof(T));
        auto *value = new (storage) T(std::forward<Args>(args)...);
        destructors_.push_back(
            {value, [](void *ptr) { static_cast<T *>(ptr)->~T(); }});
        return value;
    }
};

}  // namespace lona
