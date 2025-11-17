//===- wait_any.h - Provides a wait_any interface. --------------*- C++ -*-===//
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
 * @file wait_any.h
 * @brief First-result asynchronous selector
 * Provides non-blocking race detection for multiple features with immediate
 * return on first available result
 * @note Returns immediately if any input is already satisfied
 * Uncompleted futures remain valid and must be handled separately
 */

#ifndef BISHENG_WAIT_ANY_H
#define BISHENG_WAIT_ANY_H

#include "wait_some.h"

#include <iterator>
#include <vector>

namespace bisheng {

  template <typename T>
  void wait_any(std::vector<future<T>> const &values) {
    wait_some(1, values);
  }

  template <typename T>
  inline void wait_any(std::vector<future<T>> &values) {
    wait_any(const_cast<std::vector<future<T>> const &>(values));
  }

  template <typename T>
  inline void wait_any(std::vector<future<T>> &&values) {
    wait_any(const_cast<std::vector<future<T>> const &>(values));
  }

  template <typename Iter,
            typename Enable = std::enable_if_t<is_iterator_v<Iter>>>
  void wait_any(Iter begin, Iter end) {
    wait_some(1, begin, end);
  }

  inline void wait_any() { wait_some(0); }

  template <typename... Ts>
  inline void wait_any(Ts &&...ts) {
    wait_some(1, std::forward<Ts>(ts)...);
  }

} // namespace bisheng

#endif // BISHENG_WAIT_ANY_H