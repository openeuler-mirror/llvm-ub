//===- new.h - Provides a component new interface. --------------*- C++ -*-===//
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
 * @file new.h
 * @brief Create new instances of the given Component type on the current or
 * specified locality
 * @note These functions requires to specify an explicit function on how to
 * create the component
 */

#ifndef BISHENG_NEW_H
#define BISHENG_NEW_H

#include "internal_future.h"
#include "create.h"
#include <string>

namespace bisheng {

template <typename Component, typename F, typename... Args,
  typename Enable = std::enable_if_t<
  std::is_same_v<Component *, std::invoke_result_t<F, Args...>>>>
id_type createNew(F func, Args &&...args) {
  return createComponent<Component>(func, args...);
}

template <typename Component, typename F, typename... Args,
  typename Enable = std::enable_if_t<
  std::is_same_v<Component *, std::invoke_result_t<F, Args...>>>>
id_type createNew(F func, std::string node, double value, Args &&...args) {
  return createComponent<Component>(func, node, value, args...);
}
} // namespace bisheng

#endif // BISHENG_NEW_H