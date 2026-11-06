// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <utility>
#include <unistd.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "core/common/api/hw_context_int.h"
#include "core/common/config_reader.h"
#include "core/common/message.h"
#include "core/common/shim/hwctx_handle.h"
#include "xdp/profile/database/static_info/aie_util.h"

namespace xdp::aie::dtrace {

  namespace {
    using severity_level = xrt_core::message::severity_level;

    static constexpr unsigned int DEFAULT_COALESCE_RESULT_MEMORY_MB = 256;

    void addPortCounterPair(std::vector<L2L2CounterPoint>& points,
                            uint8_t column,
                            uint8_t portIndex,
                            uint8_t runningCounter,
                            uint8_t stalledCounter)
    {
      L2L2CounterPoint running;
      running.column = column;
      running.row = MEM_TILE_ROW_START;
      running.portIndex = portIndex;
      running.counterNumber = runningCounter;
      running.eventType = "running";
      points.push_back(running);

      L2L2CounterPoint stalled;
      stalled.column = column;
      stalled.row = MEM_TILE_ROW_START;
      stalled.portIndex = portIndex;
      stalled.counterNumber = stalledCounter;
      stalled.eventType = "stalled";
      points.push_back(stalled);
    }

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

  bool isPlioBandwidthMetric(const std::string& metricSet)
  {
    return metricSet == "plio_read_bandwidth";
  }

  std::map<int, std::vector<PlWtsProbe>> parsePlWtsJson()
  {
    std::map<int, std::vector<PlWtsProbe>> byUc;

    // Tests drop pl_wts.json in the directory they are launched from.
    // The name is a placeholder until the real file name is supplied.
    const std::filesystem::path jsonPath =
        std::filesystem::current_path() / "pl_wts.json";

    std::error_code ec;
    if (!std::filesystem::exists(jsonPath, ec)) {
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
          "AIE dtrace: PLIO task file not found: " + jsonPath.string());
      return byUc;
    }

    boost::property_tree::ptree pt;
    try {
      boost::property_tree::read_json(jsonPath.string(), pt);
    }
    catch (const std::exception& e) {
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
          std::string("AIE dtrace: failed to parse ") + jsonPath.string() + ": " + e.what());
      return byUc;
    }

    for (const auto& entry : pt) {
      int index = 0;
      try {
        index = std::stoi(entry.first);
      }
      catch (const std::exception&) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
            "AIE dtrace: ignoring non-integer PLIO task key '" + entry.first + "'.");
        continue;
      }
      if (index < 0) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
            "AIE dtrace: ignoring negative PLIO task key '" + entry.first + "'.");
        continue;
      }

      const auto& node = entry.second;
      auto linesNode = node.get_child_optional("pl_wts_config_lines");
      auto bytesNode = node.get_child_optional("pl_wts_data_bytes");
      if (!linesNode || !bytesNode) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
            "AIE dtrace: PLIO task key '" + entry.first
            + "' is missing pl_wts_config_lines or pl_wts_data_bytes.");
        continue;
      }

      std::vector<int> lines;
      std::vector<long long> bytes;
      bool ok = true;
      try {
        for (const auto& item : *linesNode)
          lines.push_back(item.second.get_value<int>());
        for (const auto& item : *bytesNode)
          bytes.push_back(item.second.get_value<long long>());
      }
      catch (const std::exception&) {
        ok = false;
      }
      if (!ok || lines.size() != bytes.size()) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
            "AIE dtrace: PLIO task key '" + entry.first
            + "' has non-integer values or pl_wts_config_lines and pl_wts_data_bytes differ in length.");
        continue;
      }

      // File key i is the i-th control program. Jprobe UC numbers are 0, 4, 8, ...
      const int ucNumber = index * 4;
      std::vector<PlWtsProbe> probes;
      long long sumBytes = 0;
      bool countsOk = true;
      for (size_t k = 0; k < lines.size(); ++k) {
        if (lines[k] <= 0 || bytes[k] < 0) {
          countsOk = false;
          break;
        }
        sumBytes += bytes[k];
        const long long stop = (sumBytes + 7) / 8;
        if (stop > static_cast<long long>(UINT32_MAX)) {
          countsOk = false;
          break;
        }
        PlWtsProbe probe;
        probe.line = lines[k];
        probe.stopCount = static_cast<uint32_t>(stop);
        probes.push_back(probe);
      }
      if (!countsOk) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
            "AIE dtrace: PLIO task key '" + entry.first
            + "' has a non-positive line, a negative byte count, or a stop count that does not fit in 32 bits.");
        continue;
      }
      byUc[ucNumber] = std::move(probes);
    }

    return byUc;
  }

  uint8_t getPlioSouthStreamPortIndex(uint8_t channel)
  {
    // south0 -> physical stream-switch port 2 (slots 0/1 are tile_control/fifo).
    static constexpr uint8_t SOUTH0_STREAM_PORT_INDEX = 2;
    return static_cast<uint8_t>(SOUTH0_STREAM_PORT_INDEX + channel);
  }

  std::vector<PlioInfoPort> parsePlioInfoJson()
  {
    std::vector<PlioInfoPort> ports;

    // Tests drop plip_info.json in the directory they are launched from.
    const std::filesystem::path jsonPath =
        std::filesystem::current_path() / "plip_info.json";

    std::error_code ec;
    if (!std::filesystem::exists(jsonPath, ec)) {
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
          "AIE dtrace: PLIO metadata file not found: " + jsonPath.string());
      return ports;
    }

    try {
      boost::property_tree::ptree pt;
      boost::property_tree::read_json(jsonPath.string(), pt);

      auto pliosItr = pt.get_child_optional("plios");
      if (!pliosItr) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
            "AIE dtrace: 'plios' section missing in " + jsonPath.string());
        return ports;
      }

      for (const auto& plio : *pliosItr) {
        const auto& node = plio.second;
        auto colStr = node.get_optional<std::string>("column");
        auto chStr  = node.get_optional<std::string>("channel");
        if (!colStr)
          continue;

        PlioInfoPort port;
        port.column  = static_cast<uint8_t>(std::stoi(*colStr));
        port.channel = chStr ? static_cast<uint8_t>(std::stoi(*chStr)) : 0;
        ports.push_back(port);
      }
    }
    catch (const std::exception& e) {
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
          std::string("AIE dtrace: failed to parse PLIO metadata: ") + e.what());
      ports.clear();
    }

    return ports;
  }

  PartitionGeometry getPartitionGeometry(void* hwctx)
  {
    PartitionGeometry geometry;
    if (!hwctx)
      return geometry;

    boost::property_tree::ptree partitions;
    try {
      partitions = xdp::aie::getAIEPartitionInfo(hwctx);
    }
    catch (const std::exception& e) {
      xrt_core::message::send(severity_level::warning, "XRT",
          std::string("AIE dtrace: Error getting partition info: ") + e.what());
      return geometry;
    }

    if (partitions.empty())
      return geometry;

    auto ctx = xrt_core::hw_context_int::create_hw_context_from_implementation(hwctx);
    const auto slotIdx = static_cast<xrt_core::hwctx_handle*>(ctx)->get_slotidx();
    const auto pid = static_cast<int>(getpid());

    for (const auto& entry : partitions) {
      const auto& partition = entry.second;
      if (partition.get<int>("pid", -1) != pid)
        continue;
      if (partition.get<std::string>("id", "") != std::to_string(slotIdx))
        continue;

      geometry.valid = true;
      geometry.startCol = static_cast<uint8_t>(partition.get<uint64_t>("start_col", 0));
      geometry.numCols = static_cast<uint32_t>(partition.get<uint64_t>("num_cols", 0));
      return geometry;
    }

    // Drivers that do not report the owning context fall back to the last
    // partition, which is what this code did before the owner was reported at
    // all. It is only correct when this process owns a single partition.
    const auto& last = partitions.back().second;
    geometry.valid = true;
    geometry.startCol = static_cast<uint8_t>(last.get<uint64_t>("start_col", 0));
    geometry.numCols = static_cast<uint32_t>(last.get<uint64_t>("num_cols", 0));

    xrt_core::message::send(severity_level::debug, "XRT",
        "AIE dtrace: Could not match a partition to hardware context slot "
        + std::to_string(slotIdx) + "; using the last reported partition (start_col="
        + std::to_string(geometry.startCol) + ", num_cols="
        + std::to_string(geometry.numCols) + ").");

    return geometry;
  }

  std::vector<L2L2InstrumentPoint> parseL2L2DesignPoints(const std::string& spec)
  {
    std::vector<L2L2InstrumentPoint> points;
    if (spec.empty())
      return points;

    // Format: {column,row:port} — column is partition-relative (0 = partition
    // start_col); row is accepted for readability only (ignored).
    static const std::regex pointRegex(R"(\{\s*(\d+)\s*,\s*(\d+)\s*:\s*(\d+)\s*\})");
    const auto begin = std::sregex_iterator(spec.begin(), spec.end(), pointRegex);
    const auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      try {
        const unsigned long column = std::stoul((*it)[1].str());
        const unsigned long dstPort = std::stoul((*it)[3].str());
        if (column > 255 || (dstPort != 1 && dstPort != 2))
          continue;

        L2L2InstrumentPoint point;
        point.column = static_cast<uint8_t>(column);
        point.dstPort = static_cast<uint8_t>(dstPort);
        points.push_back(point);
      }
      catch (const std::exception&) {
        continue;
      }
    }
    return points;
  }

  std::vector<L2L2CounterPoint> getL2L2CounterPoints(
      uint32_t numCols,
      const std::vector<L2L2InstrumentPoint>& instrumentPoints)
  {
    if (numCols == 0 || instrumentPoints.empty())
      return {};

    // Design-point columns are partition-relative (0 .. numCols-1). Counters and
    // CT addresses use the same relative column as the rest of the CT writer.
    std::vector<L2L2CounterPoint> points;
    points.reserve(instrumentPoints.size() * 2);

    // Assign counters 0-1 for the first dst path on a tile, 2-3 for the second.
    std::map<uint8_t, uint8_t> nextCounterByColumn;
    for (const auto& instrumentPoint : instrumentPoints) {
      const uint32_t column = instrumentPoint.column;
      if (column >= numCols)
        continue;

      uint8_t& nextCounter = nextCounterByColumn[instrumentPoint.column];
      if (nextCounter >= L2L2_MAX_DST_PATHS_PER_COLUMN * 2)
        continue;

      addPortCounterPair(points, instrumentPoint.column, instrumentPoint.dstPort,
                         nextCounter, static_cast<uint8_t>(nextCounter + 1));
      nextCounter = static_cast<uint8_t>(nextCounter + 2);
    }

    return points;
  }

  std::vector<MemoryConflictTile> parseMemoryConflictPoints(
      const std::string& spec,
      std::vector<std::string>* droppedEntries)
  {
    std::vector<MemoryConflictTile> tiles;
    if (spec.empty())
      return tiles;

    // Format: {column,row:bank} or {all:bank} — column partition-relative; row is the
    // memtile row. Bank "all" (or empty) monitors every bank, 0-7 a single bank; any
    // other bank value is skipped.
    static const std::regex pointRegex(
        R"(\{\s*(?:(all)|(\d+)\s*,\s*(\d+))\s*:\s*([^\s}]*)\s*\})");
    std::vector<std::pair<MemoryConflictTile, std::string>> entries;
    const auto begin = std::sregex_iterator(spec.begin(), spec.end(), pointRegex);
    const auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      try {
        MemoryConflictTile tile;
        if ((*it)[1].matched) {
          tile.allTiles = true;
        }
        else {
          const unsigned long column = std::stoul((*it)[2].str());
          const unsigned long row = std::stoul((*it)[3].str());
          if (column > 255 || row > 255)
            continue;
          tile.column = static_cast<uint8_t>(column);
          tile.row = static_cast<uint8_t>(row);
        }

        const std::string bankStr = (*it)[4].str();
        if (!bankStr.empty() && bankStr != "all") {
          if (bankStr.size() != 1 || bankStr[0] < '0'
              || bankStr[0] >= static_cast<char>('0' + NUM_MEMTILE_BANKS))
            continue;
          tile.bank = static_cast<uint8_t>(bankStr[0] - '0');
        }

        // The first valid {all:bank} replaces earlier entries; later ones are not read.
        if (tile.allTiles) {
          if (droppedEntries) {
            for (const auto& earlier : entries)
              droppedEntries->push_back(earlier.second + " (kept " + (*it)[0].str() + ")");
          }
          tiles.push_back(tile);
          return tiles;
        }

        entries.emplace_back(tile, (*it)[0].str());
      }
      catch (const std::exception&) {
        continue;
      }
    }

    std::map<std::pair<uint8_t, uint8_t>, std::string> keptEntries;
    for (const auto& [tile, entry] : entries) {
      const auto kept = keptEntries.emplace(std::make_pair(tile.column, tile.row), entry);
      if (!kept.second) {
        if (droppedEntries)
          droppedEntries->push_back(entry + " (kept " + kept.first->second + ")");
        continue;
      }
      tiles.push_back(tile);
    }
    return tiles;
  }

  std::vector<MemoryConflictTile> filterMemoryConflictTiles(
      uint32_t numCols,
      const std::vector<uint8_t>& validMemRows,
      const std::vector<MemoryConflictTile>& tiles)
  {
    std::vector<MemoryConflictTile> filtered;
    if (numCols == 0 || tiles.empty())
      return filtered;

    for (const auto& tile : tiles) {
      if (tile.allTiles) {
        for (uint32_t column = 0; column < numCols && column <= 255; ++column) {
          for (const auto row : validMemRows) {
            MemoryConflictTile expanded;
            expanded.column = static_cast<uint8_t>(column);
            expanded.row = row;
            expanded.bank = tile.bank;
            filtered.push_back(expanded);
          }
        }
        continue;
      }
      if (tile.column >= numCols)
        continue;
      if (!validMemRows.empty()
          && std::find(validMemRows.begin(), validMemRows.end(), tile.row)
               == validMemRows.end())
        continue;
      filtered.push_back(tile);
    }
    return filtered;
  }

} // namespace xdp::aie::dtrace
