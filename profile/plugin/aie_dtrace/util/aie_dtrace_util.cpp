// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"

#include "core/common/config_reader.h"
#include "core/common/message.h"
#include "core/common/json/nlohmann/json.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>

namespace xdp::aie::dtrace {

  namespace {
    using severity_level = xrt_core::message::severity_level;

    static constexpr unsigned int DEFAULT_COALESCE_RESULT_MEMORY_MB = 256;

    // Process-lifetime registry for counter metadata. Kept as a leaky singleton
    // (never destroyed) so it remains valid during static destruction, when the
    // hw context may write the coalesced JSON dump after the XDP plugin instance
    // has already been torn down.
    struct MetadataRegistry {
      std::mutex mtx;
      std::map<uint32_t, std::map<uint32_t, std::string>> bySlot; // slot -> uid -> json
      std::set<std::string> injectedFiles;
    };

    MetadataRegistry&
    registry()
    {
      static MetadataRegistry* reg = new MetadataRegistry();
      return *reg;
    }

    // Compact the verbose dtrace probe key. The engine emits the full probe
    // specifier verbatim as the JSON key, e.g.
    //   "jprobe:aie_runtime_control.asm:uc0:line8"
    // The "jprobe:" prefix and asm filename are constant boilerplate repeated in
    // every probe block of every inference. Only the microcontroller and the ASM
    // line (the op/layer correlation) are needed, so shorten to "uc0:8".
    std::string
    shortenProbeKey(const std::string& key)
    {
      static const std::regex probeRe(R"(^jprobe:.*:uc(\d+):line(\d+)$)");
      std::smatch m;
      if (std::regex_match(key, m, probeRe))
        return "uc" + m[1].str() + ":" + m[2].str();
      return key;
    }

    // Inject metadata into a single coalesced dump file. uidMap: run uid -> JSON string.
    bool
    injectFile(const std::filesystem::path& file,
               const std::map<uint32_t, std::string>& uidMap)
    {
      try {
        nlohmann::ordered_json root;
        {
          std::ifstream in(file);
          if (!in)
            return false;
          in >> root;
        }
        if (!root.is_object() || uidMap.empty())
          return false;

        // All inferences in a coalesced file share the same counter configuration,
        // so the metadata is emitted once as a top-level "counter_metadata" entry
        // (placed before the inferences) instead of being duplicated in every
        // inference's begin block. Any captured metadata works since they are
        // identical.
        auto sharedMeta =
            nlohmann::ordered_json::parse(uidMap.begin()->second, nullptr, false);

        // Rebuild the whole document: metadata first, then each inference with its
        // probe keys shortened (order preserved).
        nlohmann::ordered_json newRoot = nlohmann::ordered_json::object();
        if (!sharedMeta.is_discarded())
          newRoot["counter_metadata"] = std::move(sharedMeta);

        for (auto& item : root.items()) {
          auto& runObj = item.value();
          if (!runObj.is_object()) {
            newRoot[item.key()] = std::move(runObj);
            continue;
          }

          nlohmann::ordered_json newRun = nlohmann::ordered_json::object();
          for (auto& kv : runObj.items())
            newRun[shortenProbeKey(kv.key())] = std::move(kv.value());

          newRoot[item.key()] = std::move(newRun);
        }

        {
          std::ofstream out(file, std::ios::trunc);
          if (!out)
            return false;
          out << newRoot.dump(4) << "\n";
        }

        xrt_core::message::send(severity_level::debug, "XRT",
            "AIE dtrace: injected counter metadata into " + file.string());
        return true;
      }
      catch (const std::exception& e) {
        xrt_core::message::send(severity_level::debug, "XRT",
            std::string{"AIE dtrace: metadata injection failed (ignored): "} + e.what());
        return false;
      }
    }
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

  void
  registerCounterMetadata(uint32_t slotIdx, uint32_t runUid, const std::string& metadataJson)
  {
    if (metadataJson.empty())
      return;
    auto& reg = registry();
    std::lock_guard<std::mutex> lk(reg.mtx);
    reg.bySlot[slotIdx][runUid] = metadataJson;
  }

  void
  injectPendingMetadata()
  {
    // The coalesced JSON dump only exists when both JSON output and coalescing are on.
    if (!(xrt_core::config::get_dtrace_output_json_format()
          && xrt_core::config::get_dtrace_coalesce_result()))
      return;

    auto& reg = registry();
    std::lock_guard<std::mutex> lk(reg.mtx);
    if (reg.bySlot.empty())
      return;

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (ec)
      return;

    for (const auto& [slot, uidMap] : reg.bySlot) {
      if (uidMap.empty())
        continue;

      // core writes dtrace_dump_ctx_<slot>_<timestamp>.json; inject the newest one
      // for this slot that has not been processed yet.
      const std::regex fileRe("^dtrace_dump_ctx_" + std::to_string(slot) + "_.*\\.json$");
      std::filesystem::path newestPath;
      std::filesystem::file_time_type newest{};
      for (const auto& entry : std::filesystem::directory_iterator(cwd, ec)) {
        if (ec)
          break;
        if (!entry.is_regular_file())
          continue;
        if (!std::regex_match(entry.path().filename().string(), fileRe))
          continue;
        auto mtime = entry.last_write_time(ec);
        if (newestPath.empty() || mtime > newest) {
          newestPath = entry.path();
          newest = mtime;
        }
      }

      if (newestPath.empty())
        continue;
      if (reg.injectedFiles.count(newestPath.string()))
        continue;

      if (injectFile(newestPath, uidMap))
        reg.injectedFiles.insert(newestPath.string());
    }
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
