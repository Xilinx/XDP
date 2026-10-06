// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef PROFILING_RUNTIME_CONFIG_DOT_H
#define PROFILING_RUNTIME_CONFIG_DOT_H

#include <climits>
#include <optional>
#include <string>

#include "xdp/config.h"

// Parser for the Debug.profiling_runtime_config ini key, whose value is a
// JSON blob describing XDP runtime configuration. In practice this value is
// populated from vitisai_config.json's "profiling_runtime_config" field
// (a sibling of ai_analyzer_enhanced_profiling, inside vaiml_config) - VAIML
// forwards it into this ini key at runtime via xrt::ini::set() /
// xrtIniStringSet(). XDP never reads vitisai_config.json directly; it only
// ever sees the resulting string via xrt_core::config::get_profiling_runtime_config().
//
// control_instrumentation and event_trace are consumed by this parser.
// Neither section's mere presence enables its corresponding plugin: the
// coarse on/off switch remains Debug.aie_dtrace / Debug.aie_trace. Once that
// switch is on, this parser's sections carry the granular detail - see
// aie_trace_enabled() in xrt_coreutil (core/common/xdp/profile.cpp) for the
// load-time gate.
//
// Example blob:
//   {"control_instrumentation":{"aie_tile":"func_stalls","mem_tile":"input_ports","interface_tile":"ddr_bandwidth",
//    "memory_tile_input_ports":"{1,1:2},{5,1:1}"},"event_trace":{"tile_based_aie_tile_metrics":"all:functions"}}

namespace xdp::profiling_runtime_config {

  struct control_instrumentation_t {
    std::optional<std::string> aie_tile;       // maps to "core" module internally
    std::optional<std::string> mem_tile;       // maps to "mem_tile" module internally
    std::optional<std::string> interface_tile; // maps to "shim" module internally
    std::optional<std::string> memory_tile_input_ports; // L2-L2 {column,row:port} list
  };

  // Mirrors the AIE_trace_settings.* xrt.ini keys 1:1. When event_trace is
  // present in the JSON blob, it is authoritative for the AIE trace plugin: any
  // key omitted here falls back to the same hardcoded default the
  // corresponding xrt.ini reader would use, not to the xrt.ini value itself
  //
  // Every field already carries its final, resolved value - the default
  // shown below when the key is absent from the JSON, matching
  // core/common/config_reader.h's own AIE_trace_settings.* defaults exactly.
  // There is no "unset" state to check for (xrt_core::config accessors are
  // guaranteed to always return a value, never an error), so callers can
  // use these fields directly with no has_value()/value_or() ceremony.
  //
  // Valid keys inside "event_trace" (anything else is ignored with a
  // warning) and their defaults if omitted:
  //   start_type                          (string,       default "time")
  //   start_time                          (string,       default "0")
  //   start_iteration                     (unsigned int, default 1)
  //   start_layer                         (unsigned int, default UINT_MAX)
  //   config_one_partition                (bool,         default false)
  //   graph_based_aie_tile_metrics        (string,       default "")
  //   tile_based_aie_tile_metrics         (string,       default "")
  //   graph_based_memory_tile_metrics     (string,       default "")
  //   tile_based_memory_tile_metrics      (string,       default "")
  //   graph_based_interface_tile_metrics  (string,       default "")
  //   tile_based_interface_tile_metrics   (string,       default "")
  //   buffer_size                         (string,       default "8M")
  //   counter_scheme                      (string,       default "es2")
  //   periodic_offload                    (bool,         default true)
  //   trace_start_broadcast                (bool,         default true)
  //   reuse_buffer                        (bool,         default false)
  //   buffer_offload_interval_us          (unsigned int, default 100)
  //   file_dump_interval_s                (unsigned int, default 5)
  //   poll_timers_interval_us             (unsigned int, default 50)
  //   max_timer_samples                   (unsigned int, default 2000000)
  //   enable_system_timeline              (bool,         default true)
  //
  // Example:
  //   "event_trace": {
  //     "tile_based_aie_tile_metrics": "all:functions",
  //     "buffer_size": "8M",
  //     "periodic_offload": true
  //   }
  struct event_trace_config_t {
    std::string start_type = "time";
    std::string start_time = "0";
    unsigned int start_iteration = 1;
    unsigned int start_layer = UINT_MAX;
    bool config_one_partition = false;
    std::string graph_based_aie_tile_metrics;
    std::string tile_based_aie_tile_metrics;
    std::string graph_based_memory_tile_metrics;
    std::string tile_based_memory_tile_metrics;
    std::string graph_based_interface_tile_metrics;
    std::string tile_based_interface_tile_metrics;
    std::string buffer_size = "8M";
    std::string counter_scheme = "es2";
    bool periodic_offload = true;
    bool trace_start_broadcast = true;
    bool reuse_buffer = false;
    unsigned int buffer_offload_interval_us = 100;
    unsigned int file_dump_interval_s = 5;
    unsigned int poll_timers_interval_us = 50;
    unsigned int max_timer_samples = 2000000;
    bool enable_system_timeline = true;
  };

  // True when the xrt.ini value is non-empty and parsed successfully.
  XDP_CORE_EXPORT bool is_set();

  // True when is_set() and the blob contained a control_instrumentation object
  // with at least one recognized key (aie_tile / mem_tile / interface_tile /
  // memory_tile_input_ports).
  XDP_CORE_EXPORT bool has_control_instrumentation();

  // Returns the cached control_instrumentation view. Safe to call even when
  // has_control_instrumentation() is false (all members will be empty).
  XDP_CORE_EXPORT const control_instrumentation_t& control_instrumentation();

  // When control_instrumentation carries mem_tile or memory_tile_input_ports,
  // ports come only from the blob (requires mem_tile "input_ports"). Otherwise
  // AIE_dtrace_settings.memory_tile_input_ports from xrt.ini is used.
  XDP_CORE_EXPORT std::string resolveMemoryTileInputPorts();

  // True when is_set() and the blob contained an "event_trace" object (even
  // if empty). Note this only reflects presence in the blob - it is NOT an
  // AIE trace enablement signal by itself; see aie_trace_enabled() in
  // core/common/xdp/profile.cpp for that.
  XDP_CORE_EXPORT bool has_event_trace();

  // Returns the cached event_trace view. Safe to call even when
  // has_event_trace() is false (every field is already at its default val)
  XDP_CORE_EXPORT const event_trace_config_t& event_trace();

  // True when has_event_trace() and the JSON explicitly specified
  // "periodic_offload" (regardless of its value) - as opposed to
  // event_trace().periodic_offload merely holding its default. This is the
  // one place a caller needs "was this explicitly set" rather than the
  // resolved value: AieTraceMetadata's client-build constructor path must
  // distinguish "user explicitly asked for periodic offload" from
  // "unspecified", even though both can resolve to the same bool.
  XDP_CORE_EXPORT bool event_trace_periodic_offload_is_explicit();

  // Per-setting resolvers: the single "check the JSON blob, else check xrt.ini"
  // decision for each AIE trace setting, so every call site
  // shares one behavior instead of duplicating the branch. When
  // has_event_trace() is true, the blob (which already has defaults)
  // is authoritative and xrt.ini is not consulted.
  // otherwise the matching xrt_core::config::get_aie_trace_settings_*()
  // value is used unchanged.
  XDP_CORE_EXPORT std::string resolveStartType();
  XDP_CORE_EXPORT std::string resolveStartTime();
  XDP_CORE_EXPORT unsigned int resolveStartIteration();
  XDP_CORE_EXPORT unsigned int resolveStartLayer();
  XDP_CORE_EXPORT bool resolveConfigOnePartition();
  XDP_CORE_EXPORT std::string resolveGraphBasedAieTileMetrics();
  XDP_CORE_EXPORT std::string resolveTileBasedAieTileMetrics();
  XDP_CORE_EXPORT std::string resolveGraphBasedMemoryTileMetrics();
  XDP_CORE_EXPORT std::string resolveTileBasedMemoryTileMetrics();
  XDP_CORE_EXPORT std::string resolveGraphBasedInterfaceTileMetrics();
  XDP_CORE_EXPORT std::string resolveTileBasedInterfaceTileMetrics();
  XDP_CORE_EXPORT std::string resolveBufferSize();
  XDP_CORE_EXPORT std::string resolveCounterScheme();
  XDP_CORE_EXPORT bool resolvePeriodicOffload();
  XDP_CORE_EXPORT bool resolveTraceStartBroadcast();
  XDP_CORE_EXPORT bool resolveReuseBuffer();
  XDP_CORE_EXPORT unsigned int resolveBufferOffloadIntervalUs();
  XDP_CORE_EXPORT unsigned int resolveFileDumpIntervalS();
  XDP_CORE_EXPORT unsigned int resolvePollTimersIntervalUs();
  XDP_CORE_EXPORT unsigned int resolveMaxTimerSamples();
  XDP_CORE_EXPORT bool resolveEnableSystemTimeline();

} // namespace xdp::profiling_runtime_config

#endif
