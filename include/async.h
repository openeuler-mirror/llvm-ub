//===---------- async.h - Provides a async interface. -*- C++ -*-----------===//
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
 * @file async.h
 * @brief async implementation for distributed computing
 *
 * Provides async functions
 * - The async function can be used to schedule a task to a specific node by
 * specifying the name and value of the target node
 * - The async function can also automatically schedule a function to a node
 */

#ifndef BISHENG_ASYNC_H
#define BISHENG_ASYNC_H

#include "future.h"
#include "remote_launch.h"
#include "type_traits.h"

#define BISHENG_REMOTE BISHENG_REMOTE_LAUNCH
namespace bisheng {

// assign a task to a specific node using its name and value
template <
    typename F, typename... Args,
    typename Enable = std::enable_if_t<!std::is_member_function_pointer_v<F>>>
auto async(F f, std::string name, double value, Args... args) {
  return bisheng::future(
      std::move(bisheng::remoteLaunchAsync(f, name, value, args...)));
}

template <
    typename F, typename... Args,
    typename Enable = std::enable_if_t<!std::is_member_function_pointer_v<F>>>
auto async(F f, const char *name, double value, Args... args) {
  return bisheng::future(std::move(
      bisheng::remoteLaunchAsync(f, std::string(name), value, args...)));
}

template <auto F, typename... Args,
          typename Enable =
              std::enable_if_t<!std::is_member_function_pointer_v<decltype(F)>>>
auto async(std::string name, double value, Args... args) {
  return bisheng::future(
      std::move(bisheng::remoteLaunchAsync<F>(name, value, args...)));
}

template <auto F, typename... Args,
          typename Enable =
              std::enable_if_t<!std::is_member_function_pointer_v<decltype(F)>>>
auto async(const char *name, double value, Args... args) {
  return bisheng::future(std::move(
      bisheng::remoteLaunchAsync<F>(std::string(name), value, args...)));
}

template <typename F, typename... Args,
          typename Enable = std::enable_if_t<
              std::is_member_function_pointer_v<F> &&
              std::is_invocable_v<F, class_of_mfunction_t<F> &, Args...>>>
auto async(F func, std::string name, double value, id_type &id,
           Args &&...args) {
  return bisheng::future(
      bisheng::remoteLaunchAsync(func, name, value, id, args...));
}

template <auto F, typename... Args,
          typename Enable = std::enable_if_t<
              std::is_member_function_pointer_v<decltype(F)> &&
              std::is_invocable_v<
                  decltype(F), class_of_mfunction_t<decltype(F)> &, Args...>>>
auto async(std::string name, double value, id_type &id, Args &&...args) {
  return bisheng::future(
      bisheng::remoteLaunchAsync<F>(name, value, id, args...));
}

// automatically assign a task to a node
template <
    typename F, typename... Args,
    typename Enable = std::enable_if_t<!std::is_member_function_pointer_v<F>>>
auto async(F f, Args... args) {
  return bisheng::future(std::move(bisheng::remoteLaunchAsync(f, args...)));
}

template <auto F, typename... Args,
          typename Enable =
              std::enable_if_t<!std::is_member_function_pointer_v<decltype(F)>>>
auto async(Args... args) {
  return bisheng::future(std::move(bisheng::remoteLaunchAsync<F>(args...)));
}

template <typename F, typename... Args,
          typename Enable = std::enable_if_t<
              std::is_member_function_pointer_v<F> &&
              std::is_invocable_v<F, class_of_mfunction_t<F> &, Args...>>>
auto async(F func, id_type &id, Args &&...args) {
  return bisheng::future(bisheng::remoteLaunchAsync(func, id, args...));
}

template <auto F, typename... Args,
          typename Enable = std::enable_if_t<
              std::is_member_function_pointer_v<decltype(F)> &&
              std::is_invocable_v<
                  decltype(F), class_of_mfunction_t<decltype(F)> &, Args...>>>
auto async(id_type &id, Args &&...args) {
  return bisheng::future(bisheng::remoteLaunchAsync<F>(id, args...));
}

} // namespace bisheng

#endif // BISHENG_ASYNC_H