//===- startup_shutdown.h - Provides a init/shutdown  interface.-*- C++ -*-===//
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
 * @file startup_shutdown.h
 * @brief User-facing interface for runtime lifecycle management
 *
 * Provides simplified access to initialization/shutdown operations
 */

#ifndef BISHENG_STARTUP_SHUTDOWN_H
#define BISHENG_STARTUP_SHUTDOWN_H

#include "init_shutdown.h"
#include <memory>
#include <stdexcept>

namespace bisheng {

class StartupShutdown {
public:
  // init
  static void Init() { InitShutdown::Init(); }

  // shutdown
  static void Shutdown() noexcept { InitShutdown::Shutdown(); }

  class ScopedInstance {
  public:
    ScopedInstance() { Init(); }
    ~ScopedInstance() { Shutdown(); }

    ScopedInstance(const ScopedInstance &) = delete;
    ScopedInstance &operator=(const ScopedInstance &) = delete;
  };

private:
  StartupShutdown() = delete;
};
} // namespace bisheng

#endif // BISHENG_STARTUP_SHUTDOWN_H