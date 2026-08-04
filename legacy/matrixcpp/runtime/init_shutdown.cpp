//===---init_shutdown.cpp - Provides a init/shutdown interface.-*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "init_shutdown.h"

namespace bisheng {

void InitShutdown::Init() {
  static std::once_flag init_flag;
  std::call_once(init_flag, [] {
    ray::Init();
    if (!ray::IsInitialized()) {
      throw std::runtime_error("Init failed");
    }
  });
}

void InitShutdown::Shutdown() noexcept {
  static std::once_flag shutdown_flag;
  std::call_once(shutdown_flag, [] { ray::Shutdown(); });
}

} // namespace bisheng