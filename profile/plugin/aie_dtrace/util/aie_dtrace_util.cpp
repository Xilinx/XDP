// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"

#include "core/common/config_reader.h"
#include "core/common/message.h"

#include <mutex>

namespace xdp::aie::dtrace {

  namespace {
    using severity_level = xrt_core::message::severity_level;

    static constexpr unsigned int DEFAULT_COALESCE_RESULT_MEMORY_MB = 256;
  } // anonymous namespace

  void
  initDtraceOutputConfig()
  {
    static std::once_flag once;
    std::call_once(once, []() {
      static constexpr const char* k_json = "Debug.dtrace_output_json_format";
      static constexpr const char* k_coalesce = "Debug.dtrace_coalesce_result";
      static constexpr const char* k_coalesce_mb = "Debug.dtrace_coalesce_result_memory_mb";

      try {
        const auto ini = xrt_core::config::detail::get_ini_values();
        auto already_set = [&](const char* key) {
          if (!xrt_core::config::detail::get_env_value(key).empty())
            return true;
          return ini.find(key) != ini.end();
        };

        // xrt.ini / env win. Fill in only keys the user did not specify.
        if (!already_set(k_json))
          xrt_core::config::detail::set(k_json, "true");
        if (!already_set(k_coalesce))
          xrt_core::config::detail::set(k_coalesce, "true");
        if (!already_set(k_coalesce_mb))
          xrt_core::config::detail::set(k_coalesce_mb,
                                        std::to_string(DEFAULT_COALESCE_RESULT_MEMORY_MB));
      }
      catch (const std::exception& e) {
        xrt_core::message::send(severity_level::warning, "XRT",
            std::string("AIE dtrace: could not apply default dtrace output settings: ")
            + e.what());
        return;
      }

      const bool json = xrt_core::config::get_dtrace_output_json_format();
      const bool coalesce = xrt_core::config::get_dtrace_coalesce_result();
      if (json && coalesce) {
        xrt_core::message::send(severity_level::info, "XRT",
            "AIE dtrace: JSON dtrace_dump with coalesced results "
            "(dtrace_dump_ctx_<slot>_<timestamp>.json on hw context teardown)");
      }
      else if (json) {
        xrt_core::message::send(severity_level::info, "XRT",
            "AIE dtrace: JSON dtrace_dump enabled (per-run dtrace_dump_ctx_*_run_*.json)");
      }
      else {
        xrt_core::message::send(severity_level::info, "XRT",
            "AIE dtrace: Python dtrace_dump enabled (per-run dtrace_dump_ctx_*_run_*.py)");
      }
    });
  }

  std::map<std::string, std::vector<XAie_Events>>
  getBandwidthInterfaceTileEventSets(int hwGen)
  {
    (void)hwGen;
    return {
      {"read_bandwidth", {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_RUNNING_1_PL}},
      {"write_bandwidth", {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_RUNNING_1_PL}},
      {"ddr_bandwidth",
       {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_RUNNING_1_PL, XAIE_EVENT_PORT_RUNNING_2_PL,
        XAIE_EVENT_PORT_RUNNING_3_PL}},
      {"peak_read_bandwidth",
       {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_STALLED_0_PL,
        XAIE_EVENT_PORT_RUNNING_1_PL, XAIE_EVENT_PORT_STALLED_1_PL}},
      {"peak_write_bandwidth",
       {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_STALLED_0_PL,
        XAIE_EVENT_PORT_RUNNING_1_PL, XAIE_EVENT_PORT_STALLED_1_PL}},
    };
  }

} // namespace xdp::aie::dtrace
