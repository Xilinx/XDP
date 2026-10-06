// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_CORE_SOURCE

#include <set>
#include <sstream>
#include <string>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "core/common/config_reader.h"
#include "core/common/message.h"

#include "xdp/profile/plugin/vp_base/profiling_runtime_config.h"

namespace xdp::profiling_runtime_config {

  using severity_level = xrt_core::message::severity_level;
  namespace pt = boost::property_tree;

  namespace {

    struct parsed_blob_t {
      bool is_set = false;
      bool has_ci = false;
      control_instrumentation_t ci{};
      bool has_et = false;
      event_trace_config_t et{};
      bool et_periodic_offload_explicit = false;
    };

    // These helpers are only called from inside get_parsed()'s cached static
    // initializer, so every message they emit fires at most once per process.
    void
    warn(const std::string& msg)
    {
      xrt_core::message::send(severity_level::warning, "XRT", msg);
    }

    void
    info(const std::string& msg)
    {
      xrt_core::message::send(severity_level::info, "XRT", msg);
    }

    // Assign field from the tree if the key is present; otherwise leave the
    // field at whatever default event_trace_config_t's member initializer
    // already gave it. No optional wrapper needed: presence is checked once,
    // here, and never again by any caller.
    template <typename T>
    void
    assign_if_present(const pt::ptree& tree, const std::string& key, T& field)
    {
      if (const auto value = tree.get_optional<T>(key))
        field = *value;
    }

    // Same as assign_if_present, but also logs the resolved value - mirrors
    // the string-field logging the original parser did (only fires
    // when the key was actually present in the JSON with a non-empty value,
    // not merely because the field's default happens to be non-empty).
    void
    assign_and_log_string(const pt::ptree& tree, const std::string& key, std::string& field)
    {
      if (const auto value = tree.get_optional<std::string>(key)) {
        field = *value;
        if (!field.empty())
          info("profiling_runtime_config.event_trace." + key + "='" + field + "'");
      }
    }

    // Parse the control_instrumentation subtree: copy known string keys into
    // the returned struct and warn about any unknown keys.
    control_instrumentation_t
    parse_control_instrumentation(const pt::ptree& ci_tree)
    {
      static const std::set<std::string> known_keys{
        "aie_tile", "mem_tile", "interface_tile", "memory_tile_input_ports"
      };

      control_instrumentation_t ci;

      for (const auto& kv : ci_tree) {
        const auto& key = kv.first;
        const auto value = kv.second.get_value<std::string>("");

        if (key == "aie_tile") {
          ci.aie_tile = value;
          if (!value.empty())
            info("profiling_runtime_config.control_instrumentation.aie_tile='" + value + "'");
        }
        else if (key == "mem_tile") {
          ci.mem_tile = value;
          if (!value.empty())
            info("profiling_runtime_config.control_instrumentation.mem_tile='" + value + "'");
        }
        else if (key == "interface_tile") {
          ci.interface_tile = value;
          if (!value.empty())
            info("profiling_runtime_config.control_instrumentation.interface_tile='" + value + "'");
        }
        else if (key == "memory_tile_input_ports") {
          ci.memory_tile_input_ports = value;
          if (!value.empty())
            info("profiling_runtime_config.control_instrumentation.memory_tile_input_ports='"
                 + value + "'");
        }
        else {
          std::stringstream msg;
          msg << "Unknown key 'profiling_runtime_config.control_instrumentation."
              << key << "' ignored. Supported keys:";
          const char* sep = " ";
          for (const auto& k : known_keys) {
            msg << sep << k;
            sep = ", ";
          }
          warn(msg.str());
        }
      }

      return ci;
    }

    // Parse the event_trace subtree: copy known keys (typed per
    // event_trace_config_t) into the returned struct - anything omitted
    // keeps the struct's own hardcoded default - and warn about any unknown keys
    // Mirrors AieTraceMetadata::checkSettings()'s validSettings list for trace settings in xrt.ini
    event_trace_config_t
    parse_event_trace(const pt::ptree& et_tree)
    {
      static const std::set<std::string> known_keys{
        "start_type", "start_time", "start_iteration", "start_layer",
        "config_one_partition", "graph_based_aie_tile_metrics",
        "tile_based_aie_tile_metrics", "graph_based_memory_tile_metrics",
        "tile_based_memory_tile_metrics", "graph_based_interface_tile_metrics",
        "tile_based_interface_tile_metrics", "buffer_size", "counter_scheme",
        "periodic_offload", "trace_start_broadcast", "reuse_buffer",
        "buffer_offload_interval_us", "file_dump_interval_s",
        "poll_timers_interval_us", "max_timer_samples", "enable_system_timeline"
      };

      event_trace_config_t et;

      assign_and_log_string(et_tree, "start_type", et.start_type);
      assign_and_log_string(et_tree, "start_time", et.start_time);
      assign_if_present(et_tree, "start_iteration", et.start_iteration);
      assign_if_present(et_tree, "start_layer", et.start_layer);
      assign_if_present(et_tree, "config_one_partition", et.config_one_partition);
      assign_and_log_string(et_tree, "graph_based_aie_tile_metrics", et.graph_based_aie_tile_metrics);
      assign_and_log_string(et_tree, "tile_based_aie_tile_metrics", et.tile_based_aie_tile_metrics);
      assign_and_log_string(et_tree, "graph_based_memory_tile_metrics", et.graph_based_memory_tile_metrics);
      assign_and_log_string(et_tree, "tile_based_memory_tile_metrics", et.tile_based_memory_tile_metrics);
      assign_and_log_string(et_tree, "graph_based_interface_tile_metrics", et.graph_based_interface_tile_metrics);
      assign_and_log_string(et_tree, "tile_based_interface_tile_metrics", et.tile_based_interface_tile_metrics);
      assign_and_log_string(et_tree, "buffer_size", et.buffer_size);
      assign_and_log_string(et_tree, "counter_scheme", et.counter_scheme);
      assign_if_present(et_tree, "periodic_offload", et.periodic_offload);
      assign_if_present(et_tree, "trace_start_broadcast", et.trace_start_broadcast);
      assign_if_present(et_tree, "reuse_buffer", et.reuse_buffer);
      assign_if_present(et_tree, "buffer_offload_interval_us", et.buffer_offload_interval_us);
      assign_if_present(et_tree, "file_dump_interval_s", et.file_dump_interval_s);
      assign_if_present(et_tree, "poll_timers_interval_us", et.poll_timers_interval_us);
      assign_if_present(et_tree, "max_timer_samples", et.max_timer_samples);
      assign_if_present(et_tree, "enable_system_timeline", et.enable_system_timeline);

      for (const auto& kv : et_tree) {
        if (known_keys.find(kv.first) == known_keys.end()) {
          std::stringstream msg;
          msg << "Unknown key 'profiling_runtime_config.event_trace."
              << kv.first << "' ignored. Supported keys:";
          const char* sep = " ";
          for (const auto& k : known_keys) {
            msg << sep << k;
            sep = ", ";
          }
          warn(msg.str());
        }
      }

      return et;
    }

    // Parse the root JSON blob exactly once.
    const parsed_blob_t&
    get_parsed()
    {
      static const parsed_blob_t cached = [] {
        parsed_blob_t out;

        const std::string raw = xrt_core::config::get_profiling_runtime_config();
        if (raw.empty())
          return out;

        try {
          pt::ptree root;
          std::istringstream is(raw);
          pt::read_json(is, root);

          out.is_set = true;

          if (const auto ci_opt = root.get_child_optional("control_instrumentation")) {
            out.ci = parse_control_instrumentation(*ci_opt);
            out.has_ci = out.ci.aie_tile.has_value()
                     || out.ci.mem_tile.has_value()
                     || out.ci.interface_tile.has_value()
                     || out.ci.memory_tile_input_ports.has_value();
          }

          if (const auto et_opt = root.get_child_optional("event_trace")) {
            out.et = parse_event_trace(*et_opt);
            out.has_et = true;
            out.et_periodic_offload_explicit =
                static_cast<bool>(et_opt->get_child_optional("periodic_offload"));
            info("profiling_runtime_config.event_trace is present; "
                 "AIE trace will be configured from this JSON blob.");
          }
        }
        catch (const std::exception& ex) {
          warn(std::string("Failed to parse Debug.profiling_runtime_config "
                           "as JSON; ignoring runtime config. Details: ")
               + ex.what());
          return parsed_blob_t{};
        }

        return out;
      }();

      return cached;
    }

    // The single "check the JSON blob, else check xrt.ini" decision every
    // event_trace setting routes through.
    template <typename T>
    T
    resolve(const T& blobValue, T (*iniGetter)())
    {
      return has_event_trace() ? blobValue : iniGetter();
    }

  } // anonymous namespace

  bool
  is_set()
  {
    return get_parsed().is_set;
  }

  bool
  has_control_instrumentation()
  {
    return get_parsed().has_ci;
  }

  const control_instrumentation_t&
  control_instrumentation()
  {
    return get_parsed().ci;
  }

  std::string
  resolveMemoryTileInputPorts()
  {
    static constexpr const char* INPUT_PORTS_METRIC_SET = "input_ports";

    if (has_control_instrumentation()) {
      const auto& ci = control_instrumentation();
      const bool memTileFieldFromBlob = ci.mem_tile.has_value() && !ci.mem_tile->empty();
      const bool blobPortsSet = ci.memory_tile_input_ports.has_value()
                             && !ci.memory_tile_input_ports->empty();
      const bool memTileUsesBlob = memTileFieldFromBlob || blobPortsSet;

      if (memTileFieldFromBlob && *ci.mem_tile == INPUT_PORTS_METRIC_SET) {
        if (blobPortsSet)
          return *ci.memory_tile_input_ports;
        return {};
      }

      // Partial blob mem-tile config: do not fall back to xrt.ini ports.
      if (memTileUsesBlob)
        return {};
    }
    return xrt_core::config::get_aie_dtrace_settings_memory_tile_input_ports();
  }

  bool
  has_event_trace()
  {
    return get_parsed().has_et;
  }

  const event_trace_config_t&
  event_trace()
  {
    return get_parsed().et;
  }

  bool
  event_trace_periodic_offload_is_explicit()
  {
    return get_parsed().et_periodic_offload_explicit;
  }

  std::string
  resolveStartType()
  {
    return resolve(event_trace().start_type, xrt_core::config::get_aie_trace_settings_start_type);
  }

  std::string
  resolveStartTime()
  {
    return resolve(event_trace().start_time, xrt_core::config::get_aie_trace_settings_start_time);
  }

  unsigned int
  resolveStartIteration()
  {
    return resolve(event_trace().start_iteration, xrt_core::config::get_aie_trace_settings_start_iteration);
  }

  unsigned int
  resolveStartLayer()
  {
    return resolve(event_trace().start_layer, xrt_core::config::get_aie_trace_settings_start_layer);
  }

  bool
  resolveConfigOnePartition()
  {
    return resolve(event_trace().config_one_partition, xrt_core::config::get_aie_trace_settings_config_one_partition);
  }

  std::string
  resolveGraphBasedAieTileMetrics()
  {
    return resolve(event_trace().graph_based_aie_tile_metrics, xrt_core::config::get_aie_trace_settings_graph_based_aie_tile_metrics);
  }

  std::string
  resolveTileBasedAieTileMetrics()
  {
    return resolve(event_trace().tile_based_aie_tile_metrics, xrt_core::config::get_aie_trace_settings_tile_based_aie_tile_metrics);
  }

  std::string
  resolveGraphBasedMemoryTileMetrics()
  {
    return resolve(event_trace().graph_based_memory_tile_metrics, xrt_core::config::get_aie_trace_settings_graph_based_memory_tile_metrics);
  }

  std::string
  resolveTileBasedMemoryTileMetrics()
  {
    return resolve(event_trace().tile_based_memory_tile_metrics, xrt_core::config::get_aie_trace_settings_tile_based_memory_tile_metrics);
  }

  std::string
  resolveGraphBasedInterfaceTileMetrics()
  {
    return resolve(event_trace().graph_based_interface_tile_metrics, xrt_core::config::get_aie_trace_settings_graph_based_interface_tile_metrics);
  }

  std::string
  resolveTileBasedInterfaceTileMetrics()
  {
    return resolve(event_trace().tile_based_interface_tile_metrics, xrt_core::config::get_aie_trace_settings_tile_based_interface_tile_metrics);
  }

  std::string
  resolveBufferSize()
  {
    return resolve(event_trace().buffer_size, xrt_core::config::get_aie_trace_settings_buffer_size);
  }

  std::string
  resolveCounterScheme()
  {
    return resolve(event_trace().counter_scheme, xrt_core::config::get_aie_trace_settings_counter_scheme);
  }

  bool
  resolvePeriodicOffload()
  {
    return resolve(event_trace().periodic_offload, xrt_core::config::get_aie_trace_settings_periodic_offload);
  }

  bool
  resolveTraceStartBroadcast()
  {
    return resolve(event_trace().trace_start_broadcast, xrt_core::config::get_aie_trace_settings_trace_start_broadcast);
  }

  bool
  resolveReuseBuffer()
  {
    return resolve(event_trace().reuse_buffer, xrt_core::config::get_aie_trace_settings_reuse_buffer);
  }

  unsigned int
  resolveBufferOffloadIntervalUs()
  {
    return resolve(event_trace().buffer_offload_interval_us, xrt_core::config::get_aie_trace_settings_buffer_offload_interval_us);
  }

  unsigned int
  resolveFileDumpIntervalS()
  {
    return resolve(event_trace().file_dump_interval_s, xrt_core::config::get_aie_trace_settings_file_dump_interval_s);
  }

  unsigned int
  resolvePollTimersIntervalUs()
  {
    return resolve(event_trace().poll_timers_interval_us, xrt_core::config::get_aie_trace_settings_poll_timers_interval_us);
  }

  unsigned int
  resolveMaxTimerSamples()
  {
    return resolve(event_trace().max_timer_samples, xrt_core::config::get_aie_trace_settings_max_timer_samples);
  }

  bool
  resolveEnableSystemTimeline()
  {
    return resolve(event_trace().enable_system_timeline, xrt_core::config::get_aie_trace_settings_enable_system_timeline);
  }

} // namespace xdp::profiling_runtime_config
