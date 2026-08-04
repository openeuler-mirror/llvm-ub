// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <atomic>
#include <optional>
#include <array>
#include <functional>
#include <iostream>
#include <chrono>
#include <ctime>
#include <vector>

#include "thread.hpp"
#include "size.hpp"

namespace uballoc {

namespace stat {

struct Report {
    const char* heap;
    const char* event;
    std::optional<uint64_t> class_size;
    uint64_t count;
    
    void print() const {
        if (class_size) {
            std::cout << heap << "," << event << "," << *class_size << "," << count << "\n";
        } else {
            std::cout << heap << "," << event << ",," << count << "\n";
        }
    }
};

struct Counter {
    std::atomic<uint64_t> value{0};
    
    Counter() = default;
    Counter(Counter&& other) noexcept {
        value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    Counter& operator=(Counter&& other) noexcept {
        value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
    
    void increment_atomic() {
        value.fetch_add(1, std::memory_order_relaxed);
    }
    
    void increment() {
        uint64_t prev = value.load(std::memory_order_relaxed);
        value.store(prev + 1, std::memory_order_relaxed);
    }
    
    uint64_t load() const {
        return value.load(std::memory_order_relaxed);
    }
};

struct SloppyCounter {
    std::atomic<int64_t> value{0};
    
    std::optional<int64_t> apply(int64_t delta, int64_t threshold) {
        int64_t prev = value.load(std::memory_order_relaxed);
        int64_t next = prev + delta;
        value.store(next, std::memory_order_relaxed);
        
        if (std::abs(next) < threshold) {
            return std::nullopt;
        }
        
        value.store(0, std::memory_order_relaxed);
        return next;
    }
};

namespace process {

enum class Event {
    FaultSmall,
    FaultLarge,
    FaultHuge,
};

struct Recorder {
    Counter fault_small;
    Counter fault_large;
    Counter fault_huge;
    
    void record(Event event) {
        Counter* counter = nullptr;
        switch (event) {
            case Event::FaultSmall: counter = &fault_small; break;
            case Event::FaultLarge: counter = &fault_large; break;
            case Event::FaultHuge: counter = &fault_huge; break;
        }
        if (counter) counter->increment_atomic();
    }
    
    std::array<Report, 3> report() const {
        return {
            Report{"small", "fault", std::nullopt, fault_small.load()},
            Report{"large", "fault", std::nullopt, fault_large.load()},
            Report{"huge", "fault", std::nullopt, fault_huge.load()},
        };
    }
};

}

namespace thread {

enum class EventType {
    Bump,
    GlobalToUnsized,
    Allocate,
    UnsizedToSized,
    Free,
    SizedToUnsized,
    UnsizedToGlobal,
    Detach,
    Disown,
    Attach,
    Claim,
};

template<typename B, size_t Count>
struct StatArray {
    std::array<Counter, Count> inner;
    
    StatArray() {
        for (auto& c : inner) c = Counter{};
    }
    
    Counter& operator[](B class_) {
        return inner[class_.index()];
    }
    
    const Counter& operator[](B class_) const {
        return inner[class_.index()];
    }
};

template<typename B>
struct EventRecorder {
    Counter bump;
    Counter global_to_unsized;
    StatArray<B, B::COUNT> allocate;
    StatArray<B, B::COUNT> unsized_to_sized;
    StatArray<B, B::COUNT> free;
    StatArray<B, B::COUNT> sized_to_unsized;
    Counter unsized_to_global;
    StatArray<B, B::COUNT> detach;
    StatArray<B, B::COUNT> disown;
    StatArray<B, B::COUNT> attach;
    StatArray<B, B::COUNT> claim;
    
    EventRecorder() = default;
    
    void record(EventType event, uint64_t size = 0, B class_ = B()) {
        Counter* counter = nullptr;
        
        switch (event) {
            case EventType::Bump:
                counter = &bump;
                break;
            case EventType::GlobalToUnsized:
                counter = &global_to_unsized;
                break;
            case EventType::Allocate:
                if (auto c = B::new_from_size(size)) {
                    counter = &allocate[*c];
                }
                break;
            case EventType::UnsizedToSized:
                counter = &unsized_to_sized[class_];
                break;
            case EventType::Free:
                if (auto c = B::new_from_size(size)) {
                    counter = &free[*c];
                }
                break;
            case EventType::SizedToUnsized:
                counter = &sized_to_unsized[class_];
                break;
            case EventType::UnsizedToGlobal:
                counter = &unsized_to_global;
                break;
            case EventType::Detach:
                counter = &detach[class_];
                break;
            case EventType::Disown:
                counter = &disown[class_];
                break;
            case EventType::Attach:
                counter = &attach[class_];
                break;
            case EventType::Claim:
                counter = &claim[class_];
                break;
        }
        
        if (counter) counter->increment();
    }
    
    std::vector<Report> report() const {
        std::vector<Report> reports;
        
        reports.push_back(Report{B::NAME, "bump", std::nullopt, bump.load()});
        reports.push_back(Report{B::NAME, "global_to_unsized", std::nullopt, global_to_unsized.load()});
        reports.push_back(Report{B::NAME, "unsized_to_global", std::nullopt, unsized_to_global.load()});
        
        for (size_t i = 0; i < B::COUNT; ++i) {
            auto c = B::from_index(i);
            if (c && !c->is_zero()) {
                uint64_t sz = c->size();
                reports.push_back(Report{B::NAME, "allocate", sz, allocate.inner[i].load()});
                reports.push_back(Report{B::NAME, "unsized_to_sized", sz, unsized_to_sized.inner[i].load()});
                reports.push_back(Report{B::NAME, "free", sz, free.inner[i].load()});
                reports.push_back(Report{B::NAME, "sized_to_unsized", sz, sized_to_unsized.inner[i].load()});
                reports.push_back(Report{B::NAME, "detach", sz, detach.inner[i].load()});
                reports.push_back(Report{B::NAME, "disown", sz, disown.inner[i].load()});
                reports.push_back(Report{B::NAME, "attach", sz, attach.inner[i].load()});
                reports.push_back(Report{B::NAME, "claim", sz, claim.inner[i].load()});
            }
        }
        
        return reports;
    }
};

template<typename B>
struct MemoryRecorder {
    SloppyCounter data;
    SloppyCounter slab_local;
    SloppyCounter slab_remote;
    StatArray<B, B::COUNT> application;
    SloppyCounter global_unsized;
    SloppyCounter local_unsized;
    StatArray<B, B::COUNT> local_sized;
    StatArray<B, B::COUNT> detached;
    StatArray<B, B::COUNT> disowned;
    
    MemoryRecorder() = default;
    
    void record(EventType event, uint64_t size_val, int64_t threshold,
                std::function<void(const char*, std::optional<uint64_t>, int64_t)> apply) {
        int64_t slab = B::SLAB_SIZE;
        
        switch (event) {
            case EventType::Allocate: {
                if (auto class_ = B::new_from_size(size_val)) {
                    int64_t sz = size_val;
                    if (auto val = application[*class_].apply(sz, threshold)) {
                        apply("application", size_val, *val);
                    }
                }
                break;
            }
            case EventType::Bump: {
                int64_t batch = 1;
                int64_t sz = slab * batch;
                
                if (auto val = local_unsized.apply(sz, threshold)) {
                    apply("local_unsized", std::nullopt, *val);
                }
                if (auto val = data.apply(sz, threshold)) {
                    apply("data", std::nullopt, *val);
                }
                break;
            }
            case EventType::GlobalToUnsized: {
                if (auto val = global_unsized.apply(-slab, threshold)) {
                    apply("global_unsized", std::nullopt, *val);
                }
                if (auto val = local_unsized.apply(slab, threshold)) {
                    apply("local_unsized", std::nullopt, *val);
                }
                break;
            }
            case EventType::UnsizedToSized: {
                B class_ = B::from_index(0).value_or(B());
                if (auto val = local_unsized.apply(-slab, threshold)) {
                    apply("local_unsized", std::nullopt, *val);
                }
                if (auto val = local_sized[class_].apply(slab, threshold)) {
                    apply("local_sized", class_.size(), *val);
                }
                break;
            }
            case EventType::Free: {
                if (auto class_ = B::new_from_size(size_val)) {
                    if (auto val = application[*class_].apply(-static_cast<int64_t>(size_val), threshold)) {
                        apply("application", size_val, *val);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
};

template<typename B>
struct Recorder {
    EventRecorder<B> event_recorder;
    
    Recorder() = default;
    
    void record([[maybe_unused]] ThreadId id, EventType event, uint64_t size = 0, B class_ = B()) {
        event_recorder.record(event, size, class_);
    }
    
    std::vector<Report> report([[maybe_unused]] ThreadId id) const {
        return event_recorder.report();
    }
};

}

inline void dump([[maybe_unused]] size_t id) {
}

}

}