// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <atomic>
#include <optional>
#include <variant>

#include "packed.hpp"
#include "thread.hpp"
#include "size.hpp"
#include "data.hpp"
#include "bitset.hpp"
#include "slab.hpp"
#include "cas.hpp"
#include "cache.hpp"

namespace uballoc {

struct RecoverState {
    std::atomic<uint64_t> state_raw{0};
    
    void clear() {
        state_raw.store(0, std::memory_order_relaxed);
    }
    
    std::optional<std::pair<uint8_t, uint64_t>> load() {
        uint64_t raw = state_raw.load(std::memory_order_relaxed);
        if (raw == 0) return std::nullopt;
        uint8_t disc = (raw >> 56) & 0xFF;
        uint64_t data = raw & ((1ULL << 56) - 1);
        return std::make_pair(disc, data);
    }
    
    void store_small(uint8_t disc, uint64_t data) {
        uint64_t raw = (static_cast<uint64_t>(disc) << 56) | (data & ((1ULL << 56) - 1));
        state_raw.store(raw, std::memory_order_relaxed);
        flush(&state_raw, Invalidate::No);
        fence();
    }
    
    void store_large(uint8_t disc, uint64_t data) {
        uint64_t raw = (static_cast<uint64_t>(disc) << 56) | (data & ((1ULL << 56) - 1)) | (2ULL << 56);
        state_raw.store(raw, std::memory_order_relaxed);
        flush(&state_raw, Invalidate::No);
        fence();
    }
};

template<typename B>
struct Heap;
template<typename B>
struct HeapShared;
template<typename B>
struct HeapOwned;

inline std::atomic<size_t>& batch_bump_pop() {
    static std::atomic<size_t> val{1};
    return val;
}

template<typename B>
struct HeapState {
    uint8_t discriminant;
    uint64_t raw_data;
    
    struct UnsizedToSized {
        std::optional<SlabIndex<B>> index;
        B class_;
    };
    
    struct GlobalToUnsized {
        SlabIndex<B> index;
        Version version;
    };
    
    struct BumpToUnsized {
        std::optional<SlabIndex<B>> start;
        Version version;
    };
    
    struct UnsizedToGlobalSave {
        SlabIndex<B> index;
    };
    
    struct UnsizedToGlobal {
        SlabIndex<B> index;
        Version version;
    };
    
    struct SizedToApplication {
        SlabIndex<B> index;
        Bit block;
    };
    
    struct ApplicationToSized {
        SlabIndex<B> index;
        Bit block;
    };
    
    struct Remote {
        SlabIndex<B> index;
        Version version;
        bool last;
    };
    
    struct Detach {
        SlabIndex<B> index;
        Version version;
    };
    
    std::variant<UnsizedToSized, GlobalToUnsized, BumpToUnsized,
                 UnsizedToGlobalSave, UnsizedToGlobal,
                 SizedToApplication, ApplicationToSized, Remote, Detach> state;
    
    static HeapState unpack(uint64_t raw, uint8_t disc);
    static uint64_t pack(const HeapState& s);
};

}

#include "heap.hpp"

namespace uballoc {

template<typename B>
HeapState<B> HeapState<B>::unpack(uint64_t raw, uint8_t disc) {
    HeapState<B> hs;
    hs.discriminant = disc;
    hs.raw_data = raw;
    
    switch (disc) {
        case 0: {
            uint32_t idx_raw = raw & 0xFFFFFFFF;
            uint8_t class_raw = (raw >> 32) & 0xFF;
            hs.state = typename HeapState<B>::UnsizedToSized{
                idx_raw > 0 ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(idx_raw)) : std::nullopt,
                B::from_index(class_raw).value_or(B())
            };
            break;
        }
        case 1: {
            hs.state = typename HeapState<B>::GlobalToUnsized{
                SlabIndex<B>::unpack(raw & 0xFFFFFFFF),
                Version((raw >> 32) & 0xFFFF)
            };
            break;
        }
        case 2: {
            uint32_t start_raw = raw & 0xFFFFFFFF;
            hs.state = typename HeapState<B>::BumpToUnsized{
                start_raw > 0 ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(start_raw)) : std::nullopt,
                Version((raw >> 32) & 0xFFFF)
            };
            break;
        }
        case 3: {
            hs.state = typename HeapState<B>::UnsizedToGlobalSave{SlabIndex<B>::unpack(raw & 0xFFFFFFFF)};
            break;
        }
        case 4: {
            hs.state = typename HeapState<B>::UnsizedToGlobal{
                SlabIndex<B>::unpack(raw & 0xFFFFFFFF),
                Version((raw >> 32) & 0xFFFF)
            };
            break;
        }
        case 5: {
            hs.state = typename HeapState<B>::SizedToApplication{
                SlabIndex<B>::unpack(raw & 0xFFFFFFFF),
                Bit::unpack((raw >> 32) & 0xFFF)
            };
            break;
        }
        case 6: {
            hs.state = typename HeapState<B>::ApplicationToSized{
                SlabIndex<B>::unpack(raw & 0xFFFFFFFF),
                Bit::unpack((raw >> 32) & 0xFFF)
            };
            break;
        }
        case 7: {
            hs.state = typename HeapState<B>::Remote{
                SlabIndex<B>::unpack(raw & 0xFFFFFFFF),
                Version((raw >> 32) & 0xFFFF),
                static_cast<bool>((raw >> 48) & 0x1)
            };
            break;
        }
        case 8: {
            hs.state = typename HeapState<B>::Detach{
                SlabIndex<B>::unpack(raw & 0xFFFFFFFF),
                Version((raw >> 32) & 0xFFFF)
            };
            break;
        }
        default:
            hs.state = typename HeapState<B>::UnsizedToSized{std::nullopt, B()};
    }
    return hs;
}

template<typename B>
uint64_t HeapState<B>::pack(const HeapState<B>& hs) {
    if (std::holds_alternative<typename HeapState<B>::UnsizedToSized>(hs.state)) {
        auto& s = std::get<typename HeapState<B>::UnsizedToSized>(hs.state);
        uint32_t idx = s.index ? s.index->internal() : 0;
        return idx | (static_cast<uint64_t>(s.class_.index()) << 32);
    }
    return 0;
}

template<typename B>
void recover_heap_unsized_to_sized(ThreadId id, Heap<B>& heap,
                                   std::optional<SlabIndex<B>> index, B class_) {
    auto& unsized = heap.owned->unsized;
    
    auto peek = unsized.peek();
    if (peek && peek != index) {
        auto trace_vec = unsized.trace(*heap.slabs);
        unsized.set(peek, trace_vec.size());
    } else {
        heap.owned->unsized_to_sized(id, heap.owned->state, *heap.slabs, class_);
    }
}

template<typename B>
void recover_heap_global_to_unsized(ThreadId id, Heap<B>& heap,
                                    SlabIndex<B> index, Version version,
                                    HelpArray* help) {
    auto& unsized = heap.owned->unsized;
    
    if (heap.shared->free.head.detect(id, version, help)) {
        return;
    }
    
    unsized.recover_push(*heap.slabs, index);
}

template<typename B>
void recover_heap_global_to_unsized_distributed(ThreadId id, Heap<B>& heap,
                                                   SlabIndex<B> index, Version version,
                                                   DistributedHelpArray* help) {
    auto& unsized = heap.owned->unsized;

    if (heap.shared->free.head.detect_distributed(id, version, help)) {
        return;
    }

    unsized.recover_push(*heap.slabs, index);
}

template<typename B>
void recover_heap_bump_to_unsized(ThreadId id, Heap<B>& heap,
                                   [[maybe_unused]] std::optional<SlabIndex<B>> start, [[maybe_unused]] Version version) {
    SlabIndex<B> start_idx = start ? *start : SlabIndex<B>::min();
    
    size_t batch = batch_bump_pop().load(std::memory_order_relaxed);
    SlabIndex<B> end_idx = start_idx.unchecked_add(batch);
    
    heap.slabs->link(id, start_idx, end_idx, std::nullopt);
    heap.owned->unsized.set(start_idx, batch);
}

template<typename B>
void recover_heap_unsized_to_global_save([[maybe_unused]] ThreadId id, [[maybe_unused]] Heap<B>& heap, [[maybe_unused]] SlabIndex<B> index) {
    auto& unsized = heap.owned->unsized;
    auto peek = unsized.peek();
    
    if (peek && *peek == index) {
        unsized.recover_len(*heap.slabs);
    } else {
        unsized.set(index, 0);
        unsized.recover_len(*heap.slabs);
    }
}

template<typename B>
void recover_heap_unsized_to_global(ThreadId id, Heap<B>& heap,
                                    SlabIndex<B> index, Version version,
                                    HelpArray* help) {
    if (heap.shared->free.head.detect(id, version, help)) {
        return;
    }
    
    auto& unsized = heap.owned->unsized;
    unsized.set(index, 0);
    unsized.recover_len(*heap.slabs);
}

template<typename B>
void recover_heap_unsized_to_global_distributed(ThreadId id, Heap<B>& heap,
                                                   SlabIndex<B> index, Version version,
                                                   DistributedHelpArray* help) {
    if (heap.shared->free.head.detect_distributed(id, version, help)) {
        return;
    }

    auto& unsized = heap.owned->unsized;
    unsized.set(index, 0);
    unsized.recover_len(*heap.slabs);
}

template<typename B>
void recover_heap_sized_to_application([[maybe_unused]] ThreadId id, [[maybe_unused]] Heap<B>& heap,
                                         [[maybe_unused]] SlabIndex<B> index, [[maybe_unused]] Bit block) {
}

template<typename B>
void recover_heap_application_to_sized([[maybe_unused]] ThreadId id, [[maybe_unused]] Heap<B>& heap,
                                        [[maybe_unused]] SlabIndex<B> index, [[maybe_unused]] Bit block) {
}

template<typename B>
void recover_heap_remote(ThreadId id, Heap<B>& heap,
                         SlabIndex<B> index, Version version, bool last,
                         HelpArray* help) {
    auto& remote = heap.slabs->remote(index);
    
    if (!remote.detect(id, version, help)) {
        uint8_t class_raw = heap.slabs->local(index).class_.load(std::memory_order_relaxed);
        B bclass = B::from_index(class_raw).value_or(B());
        Offset<B> offset = heap.data->from_block(bclass, index, Bit());
        heap.free_offset(id, offset);
        return;
    }

    if (!last) return;

    heap.owned->unsized.recover_push(*heap.slabs, index);
    heap.unsized_to_global(id);
}

template<typename B>
void recover_heap_detach(ThreadId id, Heap<B>& heap,
                          SlabIndex<B> index, Version version,
                          HelpArray* help) {
    auto& remote = heap.slabs->remote(index);
    
    if (!remote.detect(id, version, help)) {
        uint8_t class_raw = heap.slabs->local(index).class_.load(std::memory_order_relaxed);
        B class_ = B::from_index(class_raw).value_or(B());
        (void)class_;
        
        auto owner = heap.slabs->local(index).get_owner();
        if (owner && *owner == id) {
            heap.slabs->local(index).disown(id);
        }
    }
}

template<typename B>
void recover_heap(ThreadId id, Heap<B>& heap, HeapState<B>& state, HelpArray* help) {
    if (std::holds_alternative<typename HeapState<B>::UnsizedToSized>(state.state)) {
        auto& s = std::get<typename HeapState<B>::UnsizedToSized>(state.state);
        recover_heap_unsized_to_sized(id, heap, s.index, s.class_);
    }
    else if (std::holds_alternative<typename HeapState<B>::GlobalToUnsized>(state.state)) {
        auto& s = std::get<typename HeapState<B>::GlobalToUnsized>(state.state);
        recover_heap_global_to_unsized(id, heap, s.index, s.version, help);
    }
    else if (std::holds_alternative<typename HeapState<B>::BumpToUnsized>(state.state)) {
        auto& s = std::get<typename HeapState<B>::BumpToUnsized>(state.state);
        recover_heap_bump_to_unsized(id, heap, s.start, s.version);
    }
    else if (std::holds_alternative<typename HeapState<B>::UnsizedToGlobalSave>(state.state)) {
        auto& s = std::get<typename HeapState<B>::UnsizedToGlobalSave>(state.state);
        recover_heap_unsized_to_global_save(id, heap, s.index);
    }
    else if (std::holds_alternative<typename HeapState<B>::UnsizedToGlobal>(state.state)) {
        auto& s = std::get<typename HeapState<B>::UnsizedToGlobal>(state.state);
        recover_heap_unsized_to_global(id, heap, s.index, s.version, help);
    }
    else if (std::holds_alternative<typename HeapState<B>::SizedToApplication>(state.state)) {
        auto& s = std::get<typename HeapState<B>::SizedToApplication>(state.state);
        recover_heap_sized_to_application(id, heap, s.index, s.block);
    }
    else if (std::holds_alternative<typename HeapState<B>::ApplicationToSized>(state.state)) {
        auto& s = std::get<typename HeapState<B>::ApplicationToSized>(state.state);
        recover_heap_application_to_sized(id, heap, s.index, s.block);
    }
    else if (std::holds_alternative<typename HeapState<B>::Remote>(state.state)) {
        auto& s = std::get<typename HeapState<B>::Remote>(state.state);
        recover_heap_remote(id, heap, s.index, s.version, s.last, help);
    }
    else if (std::holds_alternative<typename HeapState<B>::Detach>(state.state)) {
        auto& s = std::get<typename HeapState<B>::Detach>(state.state);
        recover_heap_detach(id, heap, s.index, s.version, help);
    }
}

template<typename B>
void recover_heap_remote_distributed(ThreadId id, Heap<B>& heap,
                                     SlabIndex<B> index, Version version, bool last,
                                     DistributedHelpArray* help) {
    auto& remote = heap.slabs->remote(index);

    if (!remote.detect_distributed(id, version, help)) {
        uint8_t class_raw = heap.slabs->local(index).class_.load(std::memory_order_relaxed);
        B bclass = B::from_index(class_raw).value_or(B());
        Offset<B> offset = heap.data->from_block(bclass, index, Bit());
        heap.free_offset(id, offset);
        return;
    }

    if (!last) return;

    heap.owned->unsized.recover_push(*heap.slabs, index);
    heap.unsized_to_global(id);
}

template<typename B>
void recover_heap_detach_distributed(ThreadId id, Heap<B>& heap,
                                       SlabIndex<B> index, Version version,
                                       DistributedHelpArray* help) {
    auto& remote = heap.slabs->remote(index);

    if (!remote.detect_distributed(id, version, help)) {
        auto owner = heap.slabs->local(index).get_owner();
        if (owner && *owner == id) {
            heap.slabs->local(index).disown(id);
        }
    }
}

template<typename B>
void recover_heap_distributed(ThreadId id, Heap<B>& heap, HeapState<B>& state, DistributedHelpArray* help) {
    if (std::holds_alternative<typename HeapState<B>::UnsizedToSized>(state.state)) {
        auto& s = std::get<typename HeapState<B>::UnsizedToSized>(state.state);
        recover_heap_unsized_to_sized(id, heap, s.index, s.class_);
    }
    else if (std::holds_alternative<typename HeapState<B>::GlobalToUnsized>(state.state)) {
        auto& s = std::get<typename HeapState<B>::GlobalToUnsized>(state.state);
        recover_heap_global_to_unsized_distributed(id, heap, s.index, s.version, help);
    }
    else if (std::holds_alternative<typename HeapState<B>::BumpToUnsized>(state.state)) {
        auto& s = std::get<typename HeapState<B>::BumpToUnsized>(state.state);
        recover_heap_bump_to_unsized(id, heap, s.start, s.version);
    }
    else if (std::holds_alternative<typename HeapState<B>::UnsizedToGlobalSave>(state.state)) {
        auto& s = std::get<typename HeapState<B>::UnsizedToGlobalSave>(state.state);
        recover_heap_unsized_to_global_save(id, heap, s.index);
    }
    else if (std::holds_alternative<typename HeapState<B>::UnsizedToGlobal>(state.state)) {
        auto& s = std::get<typename HeapState<B>::UnsizedToGlobal>(state.state);
        recover_heap_unsized_to_global_distributed(id, heap, s.index, s.version, help);
    }
    else if (std::holds_alternative<typename HeapState<B>::SizedToApplication>(state.state)) {
        auto& s = std::get<typename HeapState<B>::SizedToApplication>(state.state);
        recover_heap_sized_to_application(id, heap, s.index, s.block);
    }
    else if (std::holds_alternative<typename HeapState<B>::ApplicationToSized>(state.state)) {
        auto& s = std::get<typename HeapState<B>::ApplicationToSized>(state.state);
        recover_heap_application_to_sized(id, heap, s.index, s.block);
    }
    else if (std::holds_alternative<typename HeapState<B>::Remote>(state.state)) {
        auto& s = std::get<typename HeapState<B>::Remote>(state.state);
        recover_heap_remote_distributed(id, heap, s.index, s.version, s.last, help);
    }
    else if (std::holds_alternative<typename HeapState<B>::Detach>(state.state)) {
        auto& s = std::get<typename HeapState<B>::Detach>(state.state);
        recover_heap_detach_distributed(id, heap, s.index, s.version, help);
    }
}

inline void recover_allocator(ThreadId id, 
                              Heap<Small>& small_heap,
                              Heap<Large>& large_heap,
                              RecoverState& state,
                              HelpArray* help) {
    auto loaded = state.load();
    if (!loaded) return;
    
    auto [disc, data] = *loaded;
    
    if ((disc & 0xF0) == 0x10) {
        HeapState<Small> hs = HeapState<Small>::unpack(data, disc & 0x0F);
        recover_heap(id, small_heap, hs, help);
    }
    else if ((disc & 0xF0) == 0x20) {
        HeapState<Large> hs = HeapState<Large>::unpack(data, disc & 0x0F);
        recover_heap(id, large_heap, hs, help);
    }
    
    state.clear();
}

inline void recover_allocator(ThreadId id,
                              Heap<Small>& small_heap,
                              Heap<Large>& large_heap,
                              RecoverState& state,
                              DistributedHelpArray* help) {
    auto loaded = state.load();
    if (!loaded) return;

    auto [disc, data] = *loaded;

    if ((disc & 0xF0) == 0x10) {
        HeapState<Small> hs = HeapState<Small>::unpack(data, disc & 0x0F);
        recover_heap_distributed(id, small_heap, hs, help);
    }
    else if ((disc & 0xF0) == 0x20) {
        HeapState<Large> hs = HeapState<Large>::unpack(data, disc & 0x0F);
        recover_heap_distributed(id, large_heap, hs, help);
    }

    state.clear();
}

}