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

    // Parse the control_instrumentation subtree: copy known string keys into
    // the returned struct and warn about any unknown keys.
    control_instrumentation_t
    parse_control_instrumentation(const pt::ptree& ci_tree)
    {
      static const std::set<std::string> known_keys{
        "aie_tile", "mem_tile", "interface_tile"
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

    // Parse the root blob exactly once.
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
                     || out.ci.interface_tile.has_value();
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

  bool
  aie_dtrace_enabled()
  {
    static bool value = xrt_core::config::get_aie_dtrace() || has_control_instrumentation();
    return value;
  }

} // namespace xdp::profiling_runtime_config
