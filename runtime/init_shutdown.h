//===--- init_shutdown.h - Provides a init/shutdown interface -*- C++ -*---===//
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
 * @file init_shutdown.h
 * @brief Thread-safe wrapper for runtime initialization and shutdown
 *
 * Provides one-time initialization and cleanup of the runtime environment.
 * Ensures proper resource management in distributed computing scenarios.
 */

#ifndef BISHENG_INIT_SHUTDOWN_H
#define BISHENG_INIT_SHUTDOWN_H

#include <mutex>
#include <ray/api.h>
#include <stdexcept>

namespace bisheng {

class InitShutdown {
public:
  // init
  static void Init();

  // shutdown
  static void Shutdown() noexcept;

  // Prohibit instantiation
  InitShutdown() = delete;
  InitShutdown(const InitShutdown &) = delete;
  InitShutdown &operator=(const InitShutdown &) = delete;
};

} // namespace bisheng

#endif // BISHENG_INIT_SHUTDOWN_H