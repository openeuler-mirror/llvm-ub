//===- create.h - Provides a component create interface. --------*- C++ -*-===//
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
 * @file create.h
 * @brief Create new instances of the given Component type on the current or
 * specified locality
 * @note These functions require specifying an explicit funtion on how to create
 * the component
 */

#ifndef BISHENG_CREATE_H
#define BISHENG_CREATE_H

#include "internal_future.h"
#include <string>
#include <type_traits>

namespace bisheng {

template <typename Component, typename F, typename... Args,
  typename Enable = std::enable_if_t<
  std::is_same_v<Component *, std::invoke_result_t<F, Args...>>>>
id_type createComponent(F func, Args &&...args) {
  static_assert(std::is_same_v<Component *, std::invoke_result_t<F, Args...>>,
                "Component type should be same with Function return type!");

  ray::ActorHandle<Component> actor =
      ray::Actor(func).Remote(std::forward<Args>(args)...);
  return actor.ID();
}

template <typename Component, typename F, typename... Args,
  typename Enable = std::enable_if_t<
  std::is_same_v<Component *, std::invoke_result_t<F, Args...>>>>
id_type createComponent(F func, std::string node, double value,
                        Args &&...args) {
  static_assert(std::is_same_v<Component *, std::invoke_result_t<F, Args...>>,
                "Component type should be same with Function return type!");

  ray::ActorHandle<Component> actor = ray::Actor(func)
                                          .SetResource(node, value)
                                          .Remote(std::forward<Args>(args)...);
  return actor.ID();
}

} // namespace bisheng

#endif // BISHENG_CREATE_H