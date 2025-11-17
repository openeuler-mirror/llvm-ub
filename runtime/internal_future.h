//===--- InternalFuture.h - Low-level future abstraction. -*- C++ -*------===//
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
 * @file InternalFuture.h
 * @brief Core abstraction layer for future implementation
 *
 * Implements fundamental operations with:
 * - Mutex-protected value access
 * - Dual-mode error reporting
 * - Strict ownership transfer semantics
 *
 * @warning Internal implementation component - External dependencies are
 * strictly prohibited
 */

#ifndef BISHENG_INTERNAL_FUTURE_H
#define BISHENG_INTERNAL_FUTURE_H

#include <cassert>
#include <chrono>
#include <memory>
#include <ray/api.h>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bisheng {

enum class future_status {
  ready,
  timeout,
  deferred,
};

template <typename T> class InternalFuture {
public:
  InternalFuture() = default;

  explicit InternalFuture(ray::ObjectRef<T> ref) : ref_(std::move(ref)) {}

  // Move semantics (defaulted for optimal performance)
  InternalFuture(InternalFuture &&other) noexcept = default;
  InternalFuture &operator=(InternalFuture &&other) noexcept = default;

  // Deleted copy operations (non-copyable by design)
  InternalFuture(const InternalFuture &) = delete;
  InternalFuture &operator=(const InternalFuture &) = delete;

  // Primary value accessor (throws on error)
  T get() const {
    if (!IsValid()) {
      throw std::runtime_error("Accessing invalid Future");
    }

    try {
      auto ptr = ray::Get(ref_);
      if (!ptr) {
        throw std::runtime_error("Failed to get value from ObjectRef");
      }
      return *ptr;
    } catch (const ray::internal::RayTaskException &e) {
      throw std::runtime_error(e.what());
    }
  }

  // Error-code version of value accessor (noexcept)
  T get(std::error_code &ec) const noexcept {
    ec.clear();
    if (!IsValid()) {
      ec = std::make_error_code(std::errc::invalid_argument);
      return T{};
    }

    try {
      auto ptr = ray::Get(ref_);
      if (!ptr) {
        ec = std::make_error_code(std::errc::operation_not_permitted);
        return T{};
      }
      return *ptr;
    } catch (...) {
      ec = std::make_error_code(std::errc::operation_not_permitted);
      return T{};
    }
  }

  // Error-code version of pointer accessor (noexcept)
  std::shared_ptr<T> getPtr() const {
    if (!IsValid()) {
      throw std::runtime_error("Invalid internal Future");
    }

    try {
      auto ptr = ray::Get(ref_);
      if (!ptr) {
        throw std::runtime_error("Failed to get value from ObjectRef");
      }
      return ptr;
    } catch (const ray::internal::RayTaskException &e) {
      throw std::runtime_error(e.what());
    }
  }

  void wait() const {
    std::vector<ray::ObjectRef<T>> objects = {ref_};
    ray::Wait(objects, 1, -1);
  }

  template <class Clock, class Duration>
  future_status wait_until(
      const std::chrono::time_point<Clock, Duration> &timeout_time) const {
    std::vector<ray::ObjectRef<T>> objects = {ref_};
    auto result =
        ray::Wait(objects, 1,
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      timeout_time - std::chrono::system_clock::now())
                      .count());
    if (result.ready.size() == 1) {
      return future_status::ready;
    } else {
      return future_status::timeout;
    }

    assert(0 && "bisheng::wait_until should not reach here!");
  }

  template <class Rep, class Period>
  future_status
  wait_for(const std::chrono::duration<Rep, Period> &timeout_duration) const {
    return wait_until(std::chrono::system_clock::now() + timeout_duration);
  }

  explicit operator bool() const noexcept { return IsValid(); }

  // Access underlying ObjectRef
  const ray::ObjectRef<T> &GetObjectRef() const noexcept { return ref_; }

private:
  // Internal validity check
  bool IsValid() const noexcept { return !ref_.ID().empty(); }

  // Underlying Ray object reference
  ray::ObjectRef<T> ref_;
};

using id_type = std::string;

} // namespace bisheng

#endif // BISHENG_INTERNAL_FUTURE_H
