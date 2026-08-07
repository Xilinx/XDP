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
      try {
        xrt_core::config::detail::set("Debug.dtrace_output_json_format", "true");
        xrt_core::config::detail::set("Debug.dtrace_coalesce_result", "true");
        xrt_core::config::detail::set("Debug.dtrace_coalesce_result_memory_mb",
                                      std::to_string(DEFAULT_COALESCE_RESULT_MEMORY_MB));
      }
      catch (const std::exception& e) {
        xrt_core::message::send(severity_level::warning, "XRT",
            std::string("AIE dtrace: could not apply default dtrace output settings: ")
            + e.what());
        return;
      }

      xrt_core::message::send(severity_level::info, "XRT",
          "AIE dtrace: enabled JSON dtrace_dump output with coalesced results "
          "(dtrace_dump_ctx_<slot>_<timestamp>.json on hw context teardown)");
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
