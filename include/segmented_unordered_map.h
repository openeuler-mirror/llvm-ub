//===- segmented_unordered_map.h - segmented container map interface. C++ ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

/**
 * @file segmented_unordered_map.h
 * @brief segmented_unordered_map is a segmented container class like stl 
 * unordered map
 */

#ifndef BISHENG_SEGMENTED_UNORDERED_MAP_H
#define BISHENG_SEGMENTED_UNORDERED_MAP_H

#include "future.h"
#include "async.h"
#include "locality.h"

#include <unordered_map>
#include <vector>
#include <string>

namespace bisheng {
namespace detail {

template<typename K, typename V>
class UnorderedMapSeg {
public:
  // static factory method
  static UnorderedMapSeg *FactoryCreate1() {
    return new UnorderedMapSeg();
  }
  static UnorderedMapSeg *FactoryCreate2(size_t size) {
    return new UnorderedMapSeg(size);
  }
  static UnorderedMapSeg *FactoryCreate3(bisheng::id_type id) {
    bisheng::future<std::unordered_map<K, V>> f =
      bisheng::async(&UnorderedMapSeg<K, V>::get_copied_data, id);
    return new UnorderedMapSeg(f.get());
  }

  UnorderedMapSeg() {}

  explicit UnorderedMapSeg(size_t size)
    : segmented_unordered_map_(size) {
  }

  UnorderedMapSeg(std::unordered_map<K, V>&& map)
    : segmented_unordered_map_(map) {}

  UnorderedMapSeg(UnorderedMapSeg const& rhs)
    : segmented_unordered_map_(rhs.segmented_unordered_map_) {
  }

  UnorderedMapSeg(UnorderedMapSeg&& rhs)
    : segmented_unordered_map_(std::move(rhs.segmented_unordered_map_)) {
  }

  UnorderedMapSeg& operator=(UnorderedMapSeg const& rhs) {
    if (this != &rhs) {
      segmented_unordered_map_ = rhs.segmented_unordered_map_;
    }
    return *this;
  }

  UnorderedMapSeg& operator=(UnorderedMapSeg&& rhs) {
    if (this != &rhs) {
      segmented_unordered_map_ =
        std::move(rhs.segmented_unordered_map_);
    }
    return *this;
  }

  std::unordered_map<K, V> get_copied_data() {
    return segmented_unordered_map_;
  }

  bool set_copied_data(std::unordered_map<K, V> d) {
    segmented_unordered_map_ = std::move(d);
    return true;
  }

  // Capacity API
  size_t size() {
    return segmented_unordered_map_.size();
  }

  size_t max_size() {
    return segmented_unordered_map_.max_size();
  }

  bool empty() {
    return segmented_unordered_map_.empty();
  }

  // Element access API
  V get_value(K const& key) {
    auto it = segmented_unordered_map_.find(key);
    if (it == segmented_unordered_map_.end()) {
      return V();
    }
    return it->second;
  }

  std::vector<V> get_values(std::vector<K> const& key) {
    std::vector<V> result;
    result.reserve(key.size());
    for (size_t i = 0; i < key.size(); i++) {
      auto it = segmented_unordered_map_.find(key[i]);
      if (it == segmented_unordered_map_.end()) {
        return std::vector<V>();
      }
      result.push_back(it->second);
    }
    return result;
  }

  // Modifiers API
  bool set_value(K const& key, V const& val) {
    segmented_unordered_map_[key] = val;
    return true;
  }

  bool set_values(std::vector<K> const& key, std::vector<V> const& val) {
    for (size_t i = 0; i < key.size(); i++) {
      segmented_unordered_map_[key[i]] = val[i];
    }
    return true;
  }

  bool clear() {
    segmented_unordered_map_.clear();
    return true;
  }

  size_t erase(K const& key) {
    return segmented_unordered_map_.erase(key);
  }
private:
  std::unordered_map<K, V> segmented_unordered_map_;
};

template<typename K, typename V>
class UnorderedMapSegClient {
public:
  UnorderedMapSegClient() {}

  UnorderedMapSegClient(bisheng::id_type const& id) : id_(id) {
    actor_handle_ = actor_serialize(id);
  }

  // Capacity API
  bisheng::future<size_t> size_async() {
    return bisheng::async(&UnorderedMapSeg<K, V>::size, id_);
  }

  size_t size() {
    return size_async().get();
  }

  // Element access API
  bisheng::future<V> get_value_async(K const& key) {
    return bisheng::async(&UnorderedMapSeg<K, V>::get_value, id_, key);
  }

  V get_value(K const& key) {
    return get_value_async(key).get();
  }

  bisheng::future<std::vector<V>> get_values_async(
    std::vector<K> const& key
  ) {
    return bisheng::async(&UnorderedMapSeg<K, V>::get_values, id_, key);
  }

  std::vector<V> get_values(std::vector<K> const& key) {
    return get_values_async(key).get();
  }

  bisheng::future<bool> set_value_async(K const& key, V&& val) {
    return bisheng::async(&UnorderedMapSeg<K, V>::set_value, id_, key, val);
  }

  bool set_value(K const& key, V&& val) {
    return set_value_async(key, std::move(val)).get();
  }

  bisheng::future<bool> set_values_async(std::vector<K> const& key,
    std::vector<V> const& val) {
    return bisheng::async(&UnorderedMapSeg<K, V>::set_values, id_, key, val);
  }
  
  bool set_values(std::vector<K> const& key, std::vector<V> const& val) {
    return set_values_async(key, val).get();
  }

  bisheng::future<size_t> erase_async(K const& key) {
    return bisheng::async(&UnorderedMapSeg<K, V>::erase, id_, key);
  }

  size_t erase(K const& key) {
    return erase_async(key).get();
  }

  bisheng::future<std::unordered_map<K, V>> get_data() {
    return bisheng::async(&UnorderedMapSeg<K, V>::get_copied_data, id_);
  }

  bisheng::future<bool> set_data(std::unordered_map<K, V>&& d) {
    return bisheng::async(&UnorderedMapSeg<K, V>::set_copied_data, id_,
      std::move(d));
  }

  BISHENG_PACK_DEFINE(id_, actor_handle_);

  bisheng::id_type id_;
  std::string actor_handle_;
};

template<typename K>
size_t get_segment_index(K const& key, size_t segment_num) {
  return std::hash<K>{}(key) % segment_num;
}

template<typename K, typename V>
size_t size_helper(std::vector<UnorderedMapSegClient<K, V>> map) {
  if (map.empty()) {
    return 0;
  }
  std::vector<bisheng::future<size_t>> part_futures;
  for (size_t i = 0; i < map.size(); i++) {
    part_futures.push_back(
      UnorderedMapSegClient<K, V>(actor_deserialize(map[i].actor_handle_))
        .size_async()
    );
  }
  bisheng::wait_all(part_futures);
  size_t total = 0;
  for (size_t i = 0; i < part_futures.size(); i++) {
    total += part_futures[i].get();
  }
  return total;
}
}

template<typename K, typename V>
class segmented_unordered_map {
public:
  segmented_unordered_map() {
    // default policy
    std::vector<std::string> all_resources = Locality::get_all_resource();
    size_t nodes_num = all_resources.size();
    map_.reserve(nodes_num);
    for (size_t i = 0; i < nodes_num; i++) {
      bisheng::id_type id =
        bisheng::createNew<detail::UnorderedMapSeg<K, V>>(
          detail::UnorderedMapSeg<K, V>::FactoryCreate1,
          all_resources[i], 1);
      map_.push_back(detail::UnorderedMapSegClient<K, V>(id));
    }
  }

  explicit segmented_unordered_map(size_t bucket_count) {
    // default policy
    std::vector<std::string> all_resources = Locality::get_all_resource();
    size_t nodes_num = all_resources.size();
    map_.reserve(nodes_num);
    for (size_t i = 0; i < nodes_num; i++) {
      bisheng::id_type id =
        bisheng::createNew<detail::UnorderedMapSeg<K, V>>(
          detail::UnorderedMapSeg<K, V>::FactoryCreate2,
          all_resources[i], 1, bucket_count);
      map_.push_back(detail::UnorderedMapSegClient<K, V>(id));
    }
  }

  segmented_unordered_map(segmented_unordered_map const& rhs) {
    copy(rhs);
  }

  segmented_unordered_map(segmented_unordered_map&& rhs) {
    map_ = std::move(rhs.map_);
  }

  segmented_unordered_map& operator=(segmented_unordered_map const& rhs) {
    if (this != &rhs) {
      copy(rhs);
    }
  }

  segmented_unordered_map& operator=(segmented_unordered_map&& rhs) {
    if (this != &rhs) {
      map_ = std::move(rhs.map_);
    }
    return *this;
  }

  V operator[](K const& key) {
    return get_value(key);
  }

  V get_value(K const& key) {
    return get_value(detail::get_segment_index<K>(key, map_.size()), key);
  }

  V get_value(size_t seg, K const& key) {
    return map_[seg].get_value(key);
  }

  bisheng::future<V> get_value_async(K const& key) {
    return get_value_async(detail::get_segment_index<K>(key, map_.size()), key);
  }

  bisheng::future<V> get_value_async(size_t seg, K const& key) {
    return map_[seg].get_value_async(key);
  }

  bool set_value(K const& key, V&& val) {
    return set_value(detail::get_segment_index<K>(key, map_.size()),
      key, std::move(val));
  }

  bool set_value(size_t seg, K const& key, V&& val) {
    return map_[seg].set_value(key, std::move(val));
  }

  bisheng::future<bool> set_value_async(size_t seg, K const& key, V&& val) {
    return map_[seg].set_value_async(key, std::move(val));
  }

  bisheng::future<bool> set_value_async(K const& key, V&& val) {
    return set_value_async(detail::get_segment_index<K>(key, map_.size()),
      key, std::move(val));
  }

  bisheng::future<size_t> size_async() {
    return bisheng::async(detail::size_helper<K, V>, map_);
  }

  size_t size() {
    return size_async().get();
  }

  bisheng::future<size_t> erase_async(K const& key) {
    return erase_async(detail::get_segment_index<K>(key, map_.size()), key);
  }

  bisheng::future<size_t> erase_async(size_t seg, K const& key) {
    return map_[seg].erase_async(key);
  }

  size_t erase(size_t seg, K const& key) {
    return map_[seg].erase(key);
  }

  size_t erase(K const& key) {
    return erase(detail::get_segment_index<K>(key, map_.size()), key);
  }

private:
  void copy(segmented_unordered_map const& rhs) {
    std::vector<detail::UnorderedMapSegClient<K, V>> tmp_map;
    tmp_map.reserve(map_.size());
    for (size_t i = 0; i < rhs.map_.size(); i++) {
    bisheng::id_type id = bisheng::createNew<detail::UnorderedMapSeg<K, V>>(
        detail::UnorderedMapSeg<K, V>::FactoryCreate3, rhs.map_[i].id_);
    tmp_map.push_back(detail::UnorderedMapSegClient<K, V>(id));
    }
    std::swap(map_, tmp_map);
  }
  std::vector<detail::UnorderedMapSegClient<K, V>> map_;
};

template<typename K, typename V>
inline void register_segmented_unordered_map_functions();
}

#define BISHENG_DECLARE_REMOTE_SEGMENTED_UNORDERED_MAP1(K, V)               \
template<>                                                                  \
inline void bisheng::register_segmented_unordered_map_functions<K, V>() {   \
bisheng::register_function(                                           \
  "bisheng::detail::size_helper<" #K "," #V ">",                      \
  bisheng::detail::size_helper<K, V>);                                \
bisheng::register_function(                                           \
  "bisheng::detail::UnorderedMapSeg<" #K "," #V ">::FactoryCreate1",  \
  bisheng::detail::UnorderedMapSeg<K, V>::FactoryCreate1);            \
bisheng::register_function(                                           \
  "bisheng::detail::UnorderedMapSeg<" #K "," #V ">::FactoryCreate2",  \
  bisheng::detail::UnorderedMapSeg<K, V>::FactoryCreate2);            \
bisheng::register_function(                                           \
  "bisheng::detail::UnorderedMapSeg<" #K "," #V ">::FactoryCreate3",  \
  bisheng::detail::UnorderedMapSeg<K, V>::FactoryCreate3);            \
bisheng::register_function(                                           \
  "&bisheng::detail::UnorderedMapSeg<" #K "," #V ">::size",           \
  &bisheng::detail::UnorderedMapSeg<K, V>::size);                     \
bisheng::register_function(                                           \
  "&bisheng::detail::UnorderedMapSeg<" #K "," #V ">::get_value",      \
  &bisheng::detail::UnorderedMapSeg<K, V>::get_value);                \
bisheng::register_function(                                           \
  "&bisheng::detail::UnorderedMapSeg<" #K "," #V ">::set_value",      \
  &bisheng::detail::UnorderedMapSeg<K, V>::set_value);                \
bisheng::register_function(                                           \
  "&bisheng::detail::UnorderedMapSeg<" #K "," #V ">::erase",          \
  &bisheng::detail::UnorderedMapSeg<K, V>::erase);                    \
bisheng::register_function(                                           \
  "&bisheng::detail::UnorderedMapSeg<" #K "," #V ">::get_copied_data",\
  &bisheng::detail::UnorderedMapSeg<K, V>::get_copied_data);          \
bisheng::register_function(                                           \
  "&bisheng::detail::UnorderedMapSeg<" #K "," #V ">::set_copied_data",\
  &bisheng::detail::UnorderedMapSeg<K, V>::set_copied_data);          \
}                                                                     \

template<typename K, typename V>
struct register_segmented_unordered_map {
  register_segmented_unordered_map() {
    bisheng::register_segmented_unordered_map_functions<K, V>();
  }
  static register_segmented_unordered_map register_;
};
template<typename K, typename V>
register_segmented_unordered_map<K, V>
  register_segmented_unordered_map<K, V>::register_;

#define BISHENG_DECLARE_REMOTE_SEGMENTED_UNORDERED_MAP(K, V)   \
template class bisheng::detail::UnorderedMapSeg<K, V>;         \
                                                               \
BISHENG_DECLARE_REMOTE_SEGMENTED_UNORDERED_MAP1(K, V)          \
                                                               \
template register_segmented_unordered_map<K, V>                \
  register_segmented_unordered_map<K, V>::register_;           \
template class bisheng::detail::UnorderedMapSegClient<K, V>;   \
template class bisheng::segmented_unordered_map<K, V>;         \

BISHENG_DECLARE_REMOTE_SEGMENTED_UNORDERED_MAP(std::string, int)

#endif