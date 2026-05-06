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
// The parser lives on the XDP side (xdp_core) so it can evolve without
// churning the stable xrt_coreutil interface. Plugin enablement and xdp_mode
// selection are not derived from this blob: FlexmlRT (or the user) is
// expected to set Debug.aie_dtrace / Debug.aie_trace and Debug.xdp_mode in
// xrt.ini directly. The blob's role is to override the per-metric settings
// that the corresponding plugin would otherwise read from
// AIE_dtrace_settings.* / AIE_trace_settings.* in xrt.ini.
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

} // namespace xdp::profiling_runtime_config

#endif
