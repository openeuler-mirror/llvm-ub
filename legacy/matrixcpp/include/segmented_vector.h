//===- segmented_vector.h - segmented container vector interface.  C++ -*-===//
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
 * @file segmented_vector.h
 * @brief segmented_vector is a segmented container class like stl vector
 */

#ifndef BISHENG_SEGMENTED_VECTOR_H
#define BISHENG_SEGMENTED_VECTOR_H

#include "future.h"
#include "async.h"
#include "locality.h"

#include <vector>

namespace bisheng {
namespace detail {

template<typename T>
class VectorSeg {
public:
  using allocator_type = typename std::vector<T>::allocator_type;
  // static factory method
  static VectorSeg *FactoryCreate1(int num) {
    return new VectorSeg(num);
  }
  static VectorSeg *FactoryCreate2(int num, T val) {
    return new VectorSeg(num, val);
  }
  static VectorSeg *FactoryCreate3(bisheng::id_type id) {
    bisheng::future<std::vector<T>> f = bisheng::async(
      &VectorSeg<T>::get_copied_data, id
    );
    return new VectorSeg(f.get());
  }

  explicit VectorSeg(size_t size) : vector_(size) {}

  VectorSeg(size_t size, T const& val) : vector_(size, val) {}

  VectorSeg(size_t size, T const& val,
    allocator_type const& alloc) : vector_(size, val, alloc) {}

  VectorSeg(std::vector<T>&& vector) : vector_(vector) {}

  VectorSeg(VectorSeg const& rhs) = default;
  VectorSeg(VectorSeg&& rhs) = default;

  VectorSeg& operator=(VectorSeg const& rhs) = default;
  VectorSeg& operator=(VectorSeg&& rhs) = default;

  std::vector<T> get_copied_data() {
    return vector_;
  }

  std::vector<T>& get_data() {
    return vector_;
  }

  std::vector<T> const& get_data() const {
    return vector_;
  }

  bool set_data(std::vector<T> const& other) {
    vector_ = std::move(other);
    return true;
  }

  // Capacity API
  size_t size() const {
    return vector_.size();
  }

  size_t max_size() const {
    return vector_.max_size();
  }

  size_t capacity() const {
    return vector_.capacity();
  }

  bool empty() const {
    return vector_.empty();
  }

  bool resize(size_t n, T const& val) {
    vector_.resize(n, val);
    return true;
  }

  bool reserve(size_t n) {
    vector_.reserve(n);
    return true;
  }

  // Element access API
  T get_value(size_t pos) const {
    return vector_[pos];
  }

  std::vector<T> get_values(std::vector<size_t> const& pos) const {
    std::vector<T> res;
    res.reserve(pos.size());
    for (auto p : pos) {
      res.push_back(vector_[p]);
    }
    return res;
  }

  T front() const {
    return vector_.front();
  }

  T back() const {
    return vector_.back();
  }

  // Modifiers API
  bool assign(size_t n, T const& val) {
    vector_.assign(n, val);
    return true;
  }

  bool push_back(T const& val) {
    vector_.push_back(val);
    return true;
  }

  bool pop_back() {
    vector_.pop_back();
    return true;
  }

  bool set_value(size_t pos, T const& val) {
    vector_[pos] = val;
    return true;
  }

  bool set_values(std::vector<size_t> const& pos, std::vector<T> const& val) {
    for (size_t i = 0; i < pos.size(); i++) {
      vector_[pos[i]] = val[i];
    }
    return true;
  }

  bool clear() {
    vector_.clear();
    return true;
  }

private:
  std::vector<T> vector_;
};

template<typename T>
class VectorSegClient {
public:
  VectorSegClient() = default;

  explicit VectorSegClient(bisheng::id_type const& id) {
    id_ = id;
    actor_handle_ = actor_serialize(id);
  }

  // Capacity API
  bisheng::future<size_t> size_async() {
    return bisheng::async(&VectorSeg<T>::size, id_);
  }

  size_t size() {
    return size_async().get();
  }

  bisheng::future<bool> resize_async(size_t n, T const& val = T()) {
    return bisheng::async(&VectorSeg<T>::resize, id_, n, val);
  }

  bool resize(size_t n, T const& val = T()) {
    return resize_async(n, val).get();
  }

  // Element access API
  bisheng::future<T> get_value_async(size_t pos) {
    return bisheng::async(&VectorSeg<T>::get_value, id_, pos);
  }

  T get_value(size_t pos) {
    return get_value_async(pos).get();
  }

  bisheng::future<std::vector<T>> get_values_async(
    std::vector<size_t> const& pos) {
    return bisheng::async(&VectorSeg<T>::get_values, id_, pos);
  }
  
  std::vector<T> get_values(std::vector<size_t> const& pos) {
    return get_values_async(pos).get();
  }

  bisheng::future<bool> set_value_async(size_t pos, T&& val) {
    return bisheng::async(&VectorSeg<T>::set_value, id_, pos, val);
  }

  bisheng::future<bool> set_value_async(size_t pos, T const& val) {
    return bisheng::async(&VectorSeg<T>::set_value, id_, pos, val);
  }

  bool set_value(size_t pos, T&& val) {
    return set_value_async(pos, val).get();
  }

  bool set_value(size_t pos, T const& val) {
    return set_value_async(pos, val).get();
  }

  bisheng::future<bool> set_values_async(
    std::vector<size_t> pos, std::vector<T> const& val) {
    return bisheng::async(&VectorSeg<T>::set_values, id_, pos, val);
  }

  bool set_values(
    std::vector<size_t> pos, std::vector<T> const& val) {
    return set_values_async(pos, val).get();
  }
  
  bisheng::future<bool> set_data_async(std::vector<T> other) {
    return bisheng::async(&VectorSeg<T>::set_data, id_, std::move(other));
  }

  bool set_data(std::vector<T>&& other) {
    return set_data_async(std::move(other)).get();
  }

  BISHENG_PACK_DEFINE(id_, actor_handle_);

  bisheng::id_type id_;
  std::string actor_handle_;
};

inline size_t get_segment_index(size_t pos, size_t segment_size, size_t size) {
  if (segment_size != 0) {
    return (segment_size != size) ? (pos / segment_size) : 0;
  }
  return (size_t)-1;
}

inline size_t get_local_index(size_t pos, size_t segment_size, size_t size) {
  if (pos < size && segment_size != 0) {
    return (segment_size != size) ? (pos % segment_size) : pos;
  }
  return (size_t)-1;
}

template<typename T>
std::vector<T> get_values_helper(
  std::vector<VectorSegClient<T>> vector, std::vector<size_t> pos,
  size_t segment_size, size_t size
) {
  if (pos.empty()) {
    return std::vector<T>();
  }
  std::vector<std::vector<size_t>> part_indices(vector.size());
  for (size_t i = 0; i < pos.size(); i++) {
    size_t seg_index = get_segment_index(pos[i], segment_size, size);
    size_t local_index = get_local_index(pos[i], segment_size, size);
    part_indices[seg_index].push_back(local_index);
  }
  std::vector<bisheng::future<std::vector<T>>> part_futrues;
  for (size_t i = 0; i < vector.size(); i++) {
    if (part_indices[i].size() > 0) {
      part_futrues.push_back(
        VectorSegClient<T>(actor_deserialize(vector[i].actor_handle_))
          .get_values_async(part_indices[i])
      );
    }
  }
  std::vector<T> values;
  for (auto& part : part_futrues) {
    std::vector<T> part_values = part.get();
    values.insert(values.end(), part_values.begin(), part_values.end());
  }
  return values;
}

template<typename T>
bool set_values_helper(std::vector<VectorSegClient<T>> vector,
  std::vector<size_t> pos, std::vector<T> val,
  size_t segment_size, size_t size) {
  if (pos.empty()) {
    return true;
  }
  size_t segment_num = vector.size();
  std::vector<std::vector<size_t>> part_indices(segment_num);
  std::vector<std::vector<T>> part_values(segment_num);
  for (size_t i = 0; i < pos.size(); i++) {
    size_t seg_index = get_segment_index(pos[i], segment_size, size);
    size_t local_index = get_local_index(pos[i], segment_size, size);
    part_indices[seg_index].push_back(local_index);
    part_values[seg_index].push_back(val[i]);
  }
  std::vector<bisheng::future<bool>> part_futures;
  for (size_t i = 0; i < segment_num; i++) {
    if (part_indices[i].size() > 0) {
      part_futures.push_back(
        VectorSegClient<T>(actor_deserialize(vector[i].actor_handle_))
          .set_values_async(part_indices[i], std::move(part_values[i]))
      );
    }
  }
  bisheng::wait_all(part_futures);
  return true;
}
}

template<typename T>
class segmented_vector {
public:
  segmented_vector() : size_(0) {}

  segmented_vector(size_t size) : size_(size) {
    if (size != 0) {
      // default policy
      std::vector<std::string> all_resources = Locality::get_all_resource();
      size_t nodes_num = all_resources.size();
      size_t seg_size = (size + nodes_num - 1) / nodes_num;

      vector_.reserve(nodes_num);
      for (size_t i = 0; i < nodes_num; i++) {
        bisheng::id_type id =
          bisheng::createNew<detail::VectorSeg<T>>(
            detail::VectorSeg<T>::FactoryCreate1, all_resources[i], 1,
            seg_size);
        vector_.push_back(detail::VectorSegClient<T>(id));
      }
      segment_size_ = seg_size;
    }
  }

  segmented_vector(size_t size, T const& val) : size_(size) {
    if (size != 0) {
      // default policy
      std::vector<std::string> all_resources = Locality::get_all_resource();
      size_t nodes_num = all_resources.size();
      size_t seg_size = (size + nodes_num - 1) / nodes_num;

      vector_.reserve(nodes_num);
      for (size_t i = 0; i < nodes_num; i++) {
        bisheng::id_type id =
          bisheng::createNew<detail::VectorSeg<T>>(
            detail::VectorSeg<T>::FactoryCreate2, all_resources[i], 1,
            seg_size, val);
        vector_.push_back(detail::VectorSegClient<T>(id));
      }
      segment_size_ = seg_size;
    }
  }

  // copy construction
  segmented_vector(segmented_vector const& rhs) : size_(0) {
    if (rhs.size_ != 0) {
      copy(rhs);
    }
  }

  segmented_vector(segmented_vector&& rhs) {
    size_ = rhs.size_;
    segment_size_ = rhs.segment_size_;
    vector_ = std::move(rhs.vector_);
    rhs.size_ = 0;
    rhs.segment_size_ = (size_t)-1;
  }

  // Capacity API
  size_t size() const {
    return size_;
  }

  // Element access API
  T operator[](size_t pos) {
    return get_value(pos);
  }

  T get_value(size_t pos) {
    size_t seg_index = detail::get_segment_index(pos, segment_size_, size_);
    size_t local_index = detail::get_local_index(pos, segment_size_, size_);
    return vector_[seg_index].get_value(local_index);
  }

  T get_value(size_t seg, size_t pos) {
    return vector_[seg].get_value(pos);
  }

  bisheng::future<T> get_value_async(size_t pos) {
    size_t seg_index = detail::get_segment_index(pos, segment_size_, size_);
    size_t local_index = detail::get_local_index(pos, segment_size_, size_);
    return vector_[seg_index].get_value_async(local_index);
  }

  bisheng::future<T> get_value_async(size_t seg, size_t pos) {
    return vector_[seg].get_value_async(pos);
  }

  std::vector<T> get_values(size_t seg,
    std::vector<size_t> const& pos) {
    return vector_[seg].get_values(pos);
  }

  bisheng::future<std::vector<T>> get_values_async(
    size_t seg, std::vector<size_t> const& pos) {
    return vector_[seg].get_values_async(pos);
  }

  bisheng::future<std::vector<T>> get_values_async(
    std::vector<size_t> const& pos
  ) {
    return bisheng::async(detail::get_values_helper<T>, vector_, pos,
      segment_size_, size_);
  }

  std::vector<T> get_values(std::vector<size_t> const& pos) {
    return get_values_async(pos).get();
  }

  bool set_value(size_t pos, T&& val) {
    size_t seg_index = detail::get_segment_index(pos, segment_size_, size_);
    size_t local_index = detail::get_local_index(pos, segment_size_, size_);
    return vector_[seg_index].set_value(local_index, val);
  }

  bool set_value(size_t seg, size_t pos, T&& val) {
    return vector_[seg].set_value(pos, val);
  }

  bisheng::future<bool> set_value_async(size_t pos, T&& val) {
    size_t seg_index = detail::get_segment_index(pos, segment_size_, size_);
    size_t local_index = detail::get_local_index(pos, segment_size_, size_);
    return vector_[seg_index].set_value_async(local_index, val);
  }

  bisheng::future<bool> set_values_async(size_t seg,
    std::vector<size_t> const& pos, std::vector<T> const& val) {
    return vector_[seg].set_values_async(pos, val);
  }

  bisheng::future<bool> set_values_async(std::vector<size_t> const& pos,
    std::vector<T> const& val) {
    return bisheng::async(detail::set_values_helper<T>, vector_, pos, val,
      segment_size_, size_);
  }

  bool set_values(std::vector<size_t> const& pos,
    std::vector<T> const& val) {
    return set_values_async(pos, val).get();
  }

private:
  void copy(segmented_vector const& rhs) {
    std::vector<detail::VectorSegClient<T>> tmp_vector;
    tmp_vector.reserve(rhs.vector_.size());
    for (size_t i = 0; i < rhs.vector_.size(); i++) {
      bisheng::id_type id = bisheng::createNew<detail::VectorSeg<T>>(
        detail::VectorSeg<T>::FactoryCreate3, rhs.vector_[i].id_);
      tmp_vector.push_back(detail::VectorSegClient<T>(id));
    }
    size_ = rhs.size_;
    segment_size_ = rhs.segment_size_;
    std::swap(vector_, tmp_vector);
  }

  size_t size_;
  size_t segment_size_;
  std::vector<detail::VectorSegClient<T>> vector_;
};
}

// declare remote function
#define BISHENG_DECLARE_REMOTE_SEGMENTED_VECTOR(type)   \
template class bisheng::detail::VectorSeg<type>;        \
BISHENG_REMOTE(                                         \
bisheng::detail::get_values_helper<type>,               \
bisheng::detail::set_values_helper<type>,               \
bisheng::detail::VectorSeg<type>::FactoryCreate1,       \
bisheng::detail::VectorSeg<type>::FactoryCreate2,       \
bisheng::detail::VectorSeg<type>::FactoryCreate3,       \
&bisheng::detail::VectorSeg<type>::size,                \
&bisheng::detail::VectorSeg<type>::resize,              \
&bisheng::detail::VectorSeg<type>::max_size,            \
&bisheng::detail::VectorSeg<type>::capacity,            \
&bisheng::detail::VectorSeg<type>::empty,               \
&bisheng::detail::VectorSeg<type>::reserve,             \
&bisheng::detail::VectorSeg<type>::front,               \
&bisheng::detail::VectorSeg<type>::back,                \
&bisheng::detail::VectorSeg<type>::assign,              \
&bisheng::detail::VectorSeg<type>::push_back,           \
&bisheng::detail::VectorSeg<type>::pop_back,            \
&bisheng::detail::VectorSeg<type>::clear,               \
&bisheng::detail::VectorSeg<type>::get_value,           \
&bisheng::detail::VectorSeg<type>::get_values,          \
&bisheng::detail::VectorSeg<type>::set_value,           \
&bisheng::detail::VectorSeg<type>::set_values,          \
&bisheng::detail::VectorSeg<type>::set_data,            \
&bisheng::detail::VectorSeg<type>::get_copied_data      \
);                                                      \
template class bisheng::detail::VectorSegClient<type>;  \
template class bisheng::segmented_vector<type>;         \

BISHENG_DECLARE_REMOTE_SEGMENTED_VECTOR(double)
BISHENG_DECLARE_REMOTE_SEGMENTED_VECTOR(int)
BISHENG_DECLARE_REMOTE_SEGMENTED_VECTOR(std::string)
BISHENG_DECLARE_REMOTE_SEGMENTED_VECTOR(long)

#endif