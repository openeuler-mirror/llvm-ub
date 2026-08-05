// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>

namespace uballoc {

struct PublishedEntry {
    std::atomic<void*> address;
    std::atomic<size_t> size;
    std::atomic<uint32_t> type_id;
    std::atomic<uint32_t> active;
};

constexpr uint32_t PUBLISHED_ACTIVE = 1;
constexpr uint32_t PUBLISHED_INACTIVE = 0;

struct PublishedRegistry {
    static constexpr int MAX_ENTRIES = 256;
    PublishedEntry entries[MAX_ENTRIES];
    std::atomic<uint32_t> next_slot;
};

struct TypeIdMap {
    static constexpr int MAX_TYPE_IDS = 256;
    static constexpr int32_t UNOWNED = -1;
    std::atomic<int32_t> owner_process[MAX_TYPE_IDS];
};

struct PublishedInfo {
    void* address = nullptr;
    size_t size = 0;
    uint32_t type_id = 0;
    int owner_process = -1;
};

}
