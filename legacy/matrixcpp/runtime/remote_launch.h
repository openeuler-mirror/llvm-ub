//===--- remote_launch.h - Provides a remote launch interface. -*- C++ -*--===//
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
 * @file remote_launch.h
 * @brief Core abstraction layer for asynchronous execution
 *
 * Provides remote_launch functions
 * - The remote_launch function can be used to schedule a task to a specific
 * node by specifying the name and value of the target node
 * - The remote_launch function can also automatically schedule a function to
 * a node when the node is not specified
 */

#ifndef BISHENG_REMOTE_LAUNCH_H
#define BISHENG_REMOTE_LAUNCH_H

#include "internal_future.h"
#include "type_traits.h"
#include <ray/api.h>
#include <string>

#define BISHENG_REMOTE_LAUNCH RAY_REMOTE
#define BISHENG_PACK MSGPACK_DEFINE

namespace bisheng {

// assign a task to a specific node using its name and value
template <typename F, typename... Args>
auto remoteLaunchAsync(F f, std::string name, double value, Args... args) {
  return bisheng::InternalFuture(
      std::move(ray::Task(f).SetResource(name, value).Remote(args...)));
}

template <auto F, typename... Args>
auto remoteLaunchAsync(std::string name, double value, Args... args) {
  return bisheng::InternalFuture(
      std::move(ray::Task(F).SetResource(name, value).Remote(args...)));
}

// automatically assign a task to a node
template <typename F, typename... Args>
auto remoteLaunchAsync(F f, Args... args) {
  return bisheng::InternalFuture(std::move(ray::Task(f).Remote(args...)));
}

template <auto F, typename... Args> auto remoteLaunchAsync(Args... args) {
  return bisheng::InternalFuture(std::move(ray::Task(F).Remote(args...)));
}

template <typename F, typename... Args>
auto remoteLaunchAsync(F func, id_type &id, Args &&...args) {
  using ClassType = class_of_mfunction_t<F>;

  ray::ActorHandle<ClassType> actor(id);
  return bisheng::InternalFuture(
      actor.Task(func).Remote(std::forward<Args>(args)...));
}

template <auto F, typename... Args>
auto remoteLaunchAsync(id_type &id, Args &&...args) {
  using ClassType = class_of_mfunction_t<decltype(F)>;

  ray::ActorHandle<ClassType> actor(id);
  return bisheng::InternalFuture(
      actor.Task(F).Remote(std::forward<Args>(args)...));
}

template <typename F, typename... Args>
auto remoteLaunchAsync(F func, std::string name, double value, id_type &id,
                       Args &&...args) {
  using ClassType = class_of_mfunction_t<F>;

  ray::ActorHandle<ClassType> actor(id);
  return bisheng::InternalFuture(actor.Task(func)
                                     .SetResource(name, value)
                                     .Remote(std::forward<Args>(args)...));
}

template <auto F, typename... Args>
auto remoteLaunchAsync(std::string name, double value, id_type &id,
                       Args &&...args) {
  using ClassType = class_of_mfunction_t<decltype(F)>;

  ray::ActorHandle<ClassType> actor(id);
  return bisheng::InternalFuture(actor.Task(F)
                                     .SetResource(name, value)
                                     .Remote(std::forward<Args>(args)...));
}

} // namespace bisheng

#endif // BISHENG_REMOTE_LAUNCH_H
