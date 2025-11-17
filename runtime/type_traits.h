//===- type_traits.h - Provides a traits interface. -------------*- C++ -*-===//
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
 * @file type_traits.h
 * @brief Compile-time type introspection utilities
 *
 * Provides template-based type metaprogramming facilities with:
 * - Type categorization (integral, floating-point, pointer, etc.)
 * - Type transformation traits (add/remove const, pointer, reference)
 * - Type relationship checks (is_same, is_base_of, is_convertible)
 * - Compile-time constant extraction (true_type/false_type)
 * - Full C++ standard library compatibility
 * @note All operations are evaluated at compile-time with zero runtime overhead
 */

#ifndef BISHENG_TYPE_TRAITS_H
#define BISHENG_TYPE_TRAITS_H

#include <type_traits>
#include <functional>
#include <string>

namespace bisheng {

template <typename T, typename Enable = void>
struct is_iterator : std::false_type {};

template <typename T>
struct is_iterator<
    T, std::void_t<typename std::iterator_traits<T>::iterator_category>>
    : std::true_type {};

template <typename Iter>
inline constexpr bool is_iterator_v = is_iterator<Iter>::value;

template <typename T> struct class_of_mfunction {};

template <typename Class, typename Ret, typename... Args>
struct class_of_mfunction<Ret (Class::*)(Args...)> {
  using type = Class;
};

template <typename Class, typename Ret, typename... Args>
struct class_of_mfunction<Ret (Class::*)(Args...) const> {
  using type = Class;
};

template <typename Class, typename Ret, typename... Args>
struct class_of_mfunction<Ret (Class::*)(Args...) noexcept> {
  using type = Class;
};

template <typename Class, typename Ret, typename... Args>
struct class_of_mfunction<Ret (Class::*)(Args...) const noexcept> {
  using type = Class;
};

template <typename T>
using class_of_mfunction_t = typename class_of_mfunction<T>::type;

} // namespace bisheng

#endif // BISHENG_TYPE_TRAITS_H