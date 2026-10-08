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

    // The four per-inference tile keys, shared by an entry of "profile_runs"
    // and by control_instrumentation's own single-configuration form.
    const std::set<std::string>&
    tile_keys()
    {
      static const std::set<std::string> keys{
        "aie_tile", "mem_tile", "interface_tile", "memory_tile_input_ports"
      };
      return keys;
    }

    void
    warn_unknown_key(const std::string& scope, const std::string& key,
                     const std::set<std::string>& known_keys)
    {
      std::stringstream msg;
      msg << "Unknown key '" << scope << "." << key << "' ignored. Supported keys:";
      const char* sep = " ";
      for (const auto& k : known_keys) {
        msg << sep << k;
        sep = ", ";
      }
      warn(msg.str());
    }

    // Parse one entry of the "profile_runs" array. Unlike the top-level form
    // these are not logged individually; the resolved sequence is logged once
    // by the caller.
    profile_run_t
    parse_profile_run(const pt::ptree& tree, size_t index)
    {
      profile_run_t run;

      for (const auto& kv : tree) {
        const auto& key = kv.first;
        const auto value = kv.second.get_value<std::string>("");

        if (key == "aie_tile")
          run.aie_tile = value;
        else if (key == "mem_tile")
          run.mem_tile = value;
        else if (key == "interface_tile")
          run.interface_tile = value;
        else if (key == "memory_tile_input_ports")
          run.memory_tile_input_ports = value;
        else
          warn_unknown_key("profiling_runtime_config.control_instrumentation.profile_runs["
                           + std::to_string(index) + "]", key, tile_keys());
      }

      return run;
    }

    // Shorthand form: "profile_runs": "<super metric set>". A super metric set
    // names a fixed sequence of per-inference selections, so a user who wants
    // the standard report does not have to spell the sequence out.
    std::vector<profile_run_t>
    expand_super_metric_set(const std::string& name)
    {
      std::vector<profile_run_t> runs;

      if (name != "compute_io_bound") {
        warn("Unknown super metric set 'profiling_runtime_config.control_instrumentation."
             "profile_runs=" + name + "'. Supported: compute_io_bound. "
             "No profile runs configured.");
        return runs;
      }

      // Compute boundness on the first inference, then the four DDR bandwidth
      // channels that cannot share a run because they contend for the same
      // interface tile counters.
      runs.resize(4);
      runs[0].aie_tile = "compute_io_bound";
      runs[0].interface_tile = "detailed_ddr_read_bandwidth:0";
      runs[1].interface_tile = "detailed_ddr_read_bandwidth:1";
      runs[2].interface_tile = "detailed_ddr_write_bandwidth:0";
      runs[3].interface_tile = "detailed_ddr_write_bandwidth:1";

      info("profiling_runtime_config.control_instrumentation.profile_runs='" + name
           + "' expanded to " + std::to_string(runs.size()) + " inferences.");

      return runs;
    }

    // Parse "start_inference". Accepted as a JSON number or as a quoted
    // numeric string. The first inference of a kernel is 0.
    void
    parse_start_inference(const pt::ptree& node, control_instrumentation_t& ci)
    {
      const auto raw = node.get_value<std::string>("");

      try {
        ci.start_inference = node.get_value<unsigned int>();
        info("profiling_runtime_config.control_instrumentation.start_inference="
             + std::to_string(ci.start_inference));
      }
      catch (const std::exception&) {
        warn("profiling_runtime_config.control_instrumentation.start_inference='" + raw
             + "' is not an integer; starting at inference 0.");
      }
    }

    // Parse the control_instrumentation subtree: copy known string keys into
    // the returned struct and warn about any unknown keys.
    control_instrumentation_t
    parse_control_instrumentation(const pt::ptree& ci_tree)
    {
      std::set<std::string> known_keys = tile_keys();
      known_keys.insert("profile_runs");
      known_keys.insert("start_inference");

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
        else if (key == "profile_runs") {
          // A ptree node with no children is a scalar, i.e. the shorthand
          // string; one with children is the JSON array, whose elements ptree
          // exposes as children with empty keys.
          if (kv.second.empty()) {
            ci.profile_runs = expand_super_metric_set(value);
          }
          else {
            size_t index = 0;
            for (const auto& entry : kv.second)
              ci.profile_runs.push_back(parse_profile_run(entry.second, index++));
          }
          ci.has_explicit_profile_runs = !ci.profile_runs.empty();
        }
        else if (key == "start_inference") {
          parse_start_inference(kv.second, ci);
        }
        else {
          warn_unknown_key("profiling_runtime_config.control_instrumentation", key, known_keys);
        }
      }

      // Without an explicit sequence the single configuration above is itself
      // the one and only profile run, so consumers never have to branch on
      // which form the blob used.
      if (ci.profile_runs.empty()) {
        profile_run_t single;
        single.aie_tile = ci.aie_tile;
        single.mem_tile = ci.mem_tile;
        single.interface_tile = ci.interface_tile;
        single.memory_tile_input_ports = ci.memory_tile_input_ports;
        ci.profile_runs.push_back(std::move(single));
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
                     || out.ci.memory_tile_input_ports.has_value()
                     || out.ci.has_explicit_profile_runs;
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

  const std::vector<profile_run_t>&
  profile_runs()
  {
    return get_parsed().ci.profile_runs;
  }

  unsigned int
  start_inference()
  {
    return get_parsed().ci.start_inference;
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
