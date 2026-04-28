// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef PROFILING_RUNTIME_CONFIG_DOT_H
#define PROFILING_RUNTIME_CONFIG_DOT_H

#include <optional>
#include <string>

#include "xdp/config.h"

// Parser for the XRT INI option Debug.profiling_runtime_config which holds
// an inline JSON blob describing XDP runtime configuration. Only the
// control_instrumentation section is consumed by this change; other top-level
// keys (event_trace, etc.) are accepted but ignored and may be wired up in a
// follow-up.
//
// The parser lives on the XDP side (xdp_core) so this code can evolve quickly
// without churning the stable xrt_coreutil interface. core/common/xdp/profile.cpp
// keeps a minimal independent probe for the load-time gate.
//
// Example blob:
//   {"control_instrumentation":{"aie_tile":"func_stalls","mem_tile":"","interface_tile":"ddr_bandwidth"},"event_trace":{}}

namespace xdp::profiling_runtime_config {

  struct control_instrumentation_t {
    std::optional<std::string> aie_tile;       // maps to "core" module internally
    std::optional<std::string> mem_tile;       // maps to "mem_tile" module internally
    std::optional<std::string> interface_tile; // maps to "shim" module internally
  };

  // True when the xrt.ini value is non-empty and parsed successfully.
  XDP_CORE_EXPORT bool is_set();

  // True when is_set() and the blob contained a control_instrumentation object
  // with at least one recognized key (aie_tile / mem_tile / interface_tile).
  XDP_CORE_EXPORT bool has_control_instrumentation();

  // Returns the cached control_instrumentation view. Safe to call even when
  // has_control_instrumentation() is false (all members will be empty).
  XDP_CORE_EXPORT const control_instrumentation_t& control_instrumentation();

  // True if aie_dtrace should be active: either Debug.aie_dtrace is set in
  // xrt.ini, or the runtime config blob carries a control_instrumentation
  // section. Intended as the single gate for the aie_dtrace plugin guards.
  XDP_CORE_EXPORT bool aie_dtrace_enabled();

} // namespace xdp::profiling_runtime_config

#endif
