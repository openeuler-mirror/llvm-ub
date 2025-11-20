//===- wait_all.h - Provides a wait_all interface. --------------*- C++ -*-===//
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
 * @file wait_all.h
 * @brief Bulk asynchronous synchronization primitive
 * Provides concurrent waiting for multiple future objects
 * @note Input futures are consumed and become invalid after call
 * All futures must be move-constructible and share executor context
 */

#ifndef BISHENG_WAIT_ALL_H
#define BISHENG_WAIT_ALL_H

#include "future.h"
#include "type_traits.h"

#include <iterator>
#include <vector>

namespace bisheng {

  template <typename T>
  void wait_all(std::vector<future<T>> const &values) {
    for (auto &f :values) {
      f.wait();
    }
  }

  template <typename T>
  inline void wait_all(std::vector<future<T>> &values) {
    wait_all(const_cast<std::vector<future<T>> const &>(values));
  }

  template <typename T>
  inline void wait_all(std::vector<future<T>> &&values) {
    wait_all(const_cast<std::vector<future<T>> const &>(values));
  }

  template <typename Iter,
            typename Enable = std::enable_if_t<is_iterator_v<Iter>>>
  void wait_all(Iter begin, Iter end) {
    for (Iter p = begin; p != end; p++) {
      p->wait();
    }
  }

  inline void wait_all() { return; }

  template <typename T, typename... Ts>
  void wait_all(future<T> &f, Ts &&...ts) {
    f.wait();
    wait_all(ts...);
  }

} // namespace bisheng

#endif // BISHENG_WAIT_ALL_H