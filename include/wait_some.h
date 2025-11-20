//===- wait_some.h - Provides a wait_some interface. ------------*- C++ -*-===//
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
 * @file wait_some.h
 * @brief Partial completion synchronization
 * Provides waiting for a specified subset of futures to complete
 * @note Returns when at least the requested number of futures are ready
 */

#ifndef BISHENG_WAIT_SOME_H
#define BISHENG_WAIT_SOME_H

#include "future.h"
#include "type_traits.h"

#include <iterator>
#include <vector>

namespace bisheng {

  template <typename T>
  void wait_some(std::size_t n, std::vector<future<T>> const &values) {
    std::size_t size = values.size();
    std::size_t wait_num = std::min(n, size);

    std::vector<bool> removed_futures(size);
    std::size_t count = 0;
    do {
      for (std::size_t i = 0; i < size; i++) {
        auto &f = values[i];
        if (!removed_futures[i] &&
            f.wait_for(std::chrono::seconds(0)) == future_status::ready) {
          count++;
          removed_futures[i] = true;
        }
      }
    } while (count < wait_num);
  }

  template <typename T>
  inline void wait_some(std::size_t n, std::vector<future<T>> &values) {
    wait_some(n, const_cast<std::vector<future<T>> const &>(values));
  }

  template <typename T>
  inline void wait_some(std::size_t n, std::vector<future<T>> &&values) {
    return wait_some(n, const_cast<std::vector<future<T>> const &>(values));
  }

  template <typename Iter,
            typename Enable = std::enable_if_t<is_iterator_v<Iter>>>
  void wait_some(std::size_t n, Iter begin, Iter end) {
    if (begin == end) {
      return;
    }

    std::size_t count;
    do {
      count = 0;
      for (Iter p = begin; p != end; p++) {
        if (p->wait_for(std::chrono::seconds(0)) == future_status::ready) {
          count++;
        }
      }
    } while (count < n);
  }

  inline void wait_some(std::size_t n) { return; }

  template <typename T, typename... Ts>
  void wait_some(std::size_t n, future<T> &f, Ts &&...ts) {
    if (n == 0) {
      return;
    }

    std::size_t count;

    auto wait = [&](future<T> &f) {
      if (f.wait_for(std::chrono::seconds(0)) == future_status::ready) {
        count++;
      }
    };

    do {
      count = 0;
      wait(f);

      (wait(ts), ...);
    } while (count < n);
  }

} // namespace bisheng

#endif // BISHENG_WAIT_SOME_H