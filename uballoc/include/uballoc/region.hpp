// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <cstring>
#include <algorithm>

#include "error.hpp"
#include "log.hpp"

namespace uballoc {

struct RegionId {
    char buffer[64];
    size_t len;

    RegionId() : len(0) { buffer[0] = '\0'; }
    explicit RegionId(const std::string& s) : len(s.size()) {
        std::memcpy(buffer, s.c_str(), std::min(s.size(), 63UL));
        buffer[std::min(s.size(), 63UL)] = '\0';
    }

    std::string as_str() const { return std::string(buffer, len); }

    RegionId with_suffix(const std::string& suffix) const {
        RegionId result;
        size_t new_len = len + suffix.size();
        std::memcpy(result.buffer, buffer, len);
        std::memcpy(result.buffer + len, suffix.c_str(), std::min(suffix.size(), 63UL - len));
        result.len = std::min(new_len, 63UL);
        return result;
    }
};

struct Region {
    RegionId id;
    void* address;
    size_t size;
    size_t capacity;
    bool created;

    Region() : address(nullptr), size(0), capacity(0), created(false) {}

    bool is_clean() const { return created; }
};

}