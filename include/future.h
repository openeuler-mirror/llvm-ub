//===-------- future.h - Provides a future interface. -*- C++ -*-----------===//
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
 * @file future.h
 * @brief Thread-safe future implementation for distributed computing
 *
 * Provides a templated future implementation for distributed computing
 * - Type-safe asynchronous value retrieval
 * - Dual-mode error handling (exceptions/error codes)
 * - Full STL compatibility
 * - Move-only semantics for thread safety
 *
 * @note This is a move-only type that guarantees thread safety in distributed
 * environments
 * @warning External code should use the public future interface instead
 */

#ifndef BISHENG_FUTURE_H
#define BISHENG_FUTURE_H

#include "internal_future.h"
#include <chrono>
#include <memory>
#include <system_error>

namespace bisheng {

template <typename T> class future {
public:
  using result_type = T;

  future() = default;
  explicit future(bisheng::InternalFuture<T> impl) noexcept
      : impl_(std::move(impl)) {}

  // Move semantics
  future(future &&) noexcept = default;
  future &operator=(future &&) noexcept = default;

  // Deleted copy operations
  future(const future &) = delete;
  future &operator=(const future &) = delete;

  // Primary interface
  result_type get() const { return impl_.get(); }
  result_type get(std::error_code &ec) const noexcept { return impl_.get(ec); }

  // Compatibility interface (returns shared_ptr)
  std::shared_ptr<T> getPtr() const {
    if (!impl_) {
      throw std::runtime_error("Accessing invalid future");
    }
    return impl_.getPtr();
  }

  void wait() const { impl_.wait(); }

  template <class Clock, class Duration>
  future_status wait_until(
      const std::chrono::time_point<Clock, Duration> &timeout_time) const {
    return impl_.wait_until(timeout_time);
  }

  template <class Rep, class Period>
  future_status
  wait_for(const std::chrono::duration<Rep, Period> &timeout_duration) const {
    return impl_.wait_for(timeout_duration);
  }

  // Validity check
  explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

private:
  InternalFuture<T> impl_;
};
} // namespace bisheng

#endif // BISHENG_FUTURE_H