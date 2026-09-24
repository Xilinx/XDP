// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/aie_dtrace_metadata.h"

#include <algorithm>
#include <set>
#include <sstream>

#include <boost/algorithm/string.hpp>

#include "core/common/config_reader.h"
#include "core/common/message.h"
#include "xdp/profile/database/database.h"
#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"
#include "xdp/profile/plugin/vp_base/profiling_runtime_config.h"

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  static const std::set<std::string>& bandwidthMetricSets()
  {
    static const std::set<std::string> metrics = {
      "ddr_bandwidth", "read_bandwidth", "write_bandwidth",
      "peak_read_bandwidth", "peak_write_bandwidth",
      "detailed_ddr_read_bandwidth", "detailed_ddr_write_bandwidth", "off"};
    return metrics;
  }

  static const std::set<std::string>& coreMetricSets()
  {
    static const std::set<std::string> metrics = {"compute_io_bound", "off"};
    return metrics;
  }

  static constexpr const char* INPUT_PORTS_METRIC_SET = "input_ports";

  bool settingsRequestL2L2Transfer(const std::vector<std::string>& metricsSettings)
  {
    for (const auto& setting : metricsSettings) {
      std::vector<std::string> parts;
      boost::split(parts, setting, boost::is_any_of(":"));
      for (const auto& part : parts) {
        if (part == INPUT_PORTS_METRIC_SET)
          return true;
      }
    }
    return false;
  }

  AieDtraceMetadata::AieDtraceMetadata(uint64_t deviceID, void* handle)
    : deviceID(deviceID)
    , handle(handle)
  {
    xrt_core::message::send(severity_level::info, "XRT", "Parsing AIE dtrace metadata.");
    VPDatabase* db = VPDatabase::Instance();

    metadataReader = (db->getStaticInfo()).getAIEmetadataReader(deviceID);
    if (!metadataReader)
      return;

    checkDtraceSettings();
    configMetrics.resize(NUM_MODULES);
    clockFreqMhz = (db->getStaticInfo()).getClockRateMHz(deviceID, false);

    const bool usingBlob = profiling_runtime_config::has_control_instrumentation();
    const auto& ci = profiling_runtime_config::control_instrumentation();
    const auto& runs = ci.profile_runs;

    const bool useProfileRuns = usingBlob && ci.has_explicit_profile_runs && !runs.empty();
    multiInference = useProfileRuns;
    startInference = usingBlob ? ci.start_inference : 1;

    // configMetrics describes the hardware context as a whole: it is what
    // isConfigured() gates on and what createAIEProfileConfig() reports. A
    // profile_runs sequence has no single answer for it, so resolve it from the
    // first inference (falling back to any top-level key the sequence omits)
    // and let the per-inference differences live in metricSelections instead.
    const auto& effAieTile = (useProfileRuns && runs[0].aie_tile.has_value())
        ? runs[0].aie_tile : ci.aie_tile;
    const auto& effInterfaceTile = (useProfileRuns && runs[0].interface_tile.has_value())
        ? runs[0].interface_tile : ci.interface_tile;
    const auto& effMemTile = (useProfileRuns && runs[0].mem_tile.has_value())
        ? runs[0].mem_tile : ci.mem_tile;

    // Core (aie) tile metrics (e.g. compute_io_bound). Only used to enable the
    // metric; the tiles themselves are fixed to the first column.
    std::vector<std::string> aieMetricsSettings;
    if (usingBlob && effAieTile.has_value() && !effAieTile->empty()) {
      xrt_core::message::send(severity_level::info, "XRT",
          "AIE dtrace: using aie_tile metric '" + *effAieTile
          + "' from Debug.profiling_runtime_config.");
      aieMetricsSettings = getSettingsVector("all:" + *effAieTile);
    }
    else {
      const std::string tileBasedAie =
          xrt_core::config::get_aie_dtrace_settings_tile_based_aie_metrics();
      if (!tileBasedAie.empty())
        aieMetricsSettings = getSettingsVector(tileBasedAie);
    }
    getConfigMetricsForAIETiles(CORE_MODULE_IDX, aieMetricsSettings);

    std::vector<std::string> metricsSettings;
    if (usingBlob && effInterfaceTile.has_value() && !effInterfaceTile->empty()) {
      xrt_core::message::send(severity_level::info, "XRT",
          "AIE dtrace: using interface_tile metric '" + *effInterfaceTile
          + "' from Debug.profiling_runtime_config.");
      metricsSettings = getSettingsVector("all:" + *effInterfaceTile);
    }
    else {
      const std::string tileBased =
          xrt_core::config::get_aie_dtrace_settings_tile_based_interface_tile_metrics();
      if (!tileBased.empty())
        metricsSettings = getSettingsVector(tileBased);
      else
        metricsSettings = getSettingsVector("all:peak_read_bandwidth");
    }

    getConfigMetricsForInterfaceTiles(SHIM_MODULE_IDX, metricsSettings);

    // Memory tile / L2-L2: blob and xrt.ini are separate config sources. If either
    // mem_tile or memory_tile_input_ports is present and non-empty in control_instrumentation,
    // the whole mem-tile L2-L2 config must come from the blob (both fields). Otherwise
    // both tile_based_memory_tile_metrics and memory_tile_input_ports must be in xrt.ini.
    const std::string memTileSettings =
        xrt_core::config::get_aie_dtrace_settings_tile_based_memory_tile_metrics();
    const bool iniL2L2Enabled = !memTileSettings.empty()
        && settingsRequestL2L2Transfer(getSettingsVector(memTileSettings));
    const std::string iniPorts =
        xrt_core::config::get_aie_dtrace_settings_memory_tile_input_ports();
    const bool iniPortsSet = !iniPorts.empty();
    // The design points (which memory tile ports exist) are a property of the
    // design, so they stay global even when the metric sets vary per inference;
    // only whether to instrument them in a given inference is per-entry.
    const bool anyRunWantsL2L2 = useProfileRuns
        && std::any_of(runs.begin(), runs.end(), [](const auto& r) {
             return r.mem_tile.has_value() && *r.mem_tile == INPUT_PORTS_METRIC_SET;
           });

    if (useProfileRuns
        && std::any_of(runs.begin(), runs.end(), [](const auto& r) {
             return r.memory_tile_input_ports.has_value() && !r.memory_tile_input_ports->empty();
           })) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE dtrace: memory_tile_input_ports inside a profile_runs entry is not supported; "
          "design points are taken from control_instrumentation.memory_tile_input_ports "
          "(or AIE_dtrace_settings.memory_tile_input_ports) for every inference.");
    }

    const bool blobPortsSet = usingBlob && ci.memory_tile_input_ports.has_value()
                           && !ci.memory_tile_input_ports->empty();
    const bool memTileFieldFromBlob = usingBlob && effMemTile.has_value()
                                   && !effMemTile->empty();
    const bool memTileUsesBlob = usingBlob
                              && (memTileFieldFromBlob || blobPortsSet || anyRunWantsL2L2);

    bool l2L2FromBlob = false;
    if (memTileUsesBlob) {
      const bool blobEnablesL2L2 = anyRunWantsL2L2
          || (memTileFieldFromBlob && *effMemTile == INPUT_PORTS_METRIC_SET);

      if (blobEnablesL2L2) {
        l2L2TransferEnabled = true;
        l2L2FromBlob = true;
        xrt_core::message::send(severity_level::info, "XRT",
            "AIE dtrace: enabling L2-L2 via mem_tile metric '"
            + std::string(INPUT_PORTS_METRIC_SET)
            + "' from Debug.profiling_runtime_config.");
      } else if (memTileFieldFromBlob) {
        xrt_core::message::send(severity_level::info, "XRT",
            "AIE dtrace: mem tile metric '" + *effMemTile
            + "' from profiling_runtime_config will be supported in a follow-up.");
      }
    }
    else {
      l2L2TransferEnabled = iniL2L2Enabled;
    }

    if (blobPortsSet && !l2L2FromBlob) {
      xrt_core::message::send(severity_level::error, "XRT",
          "AIE dtrace: profiling_runtime_config.control_instrumentation.memory_tile_input_ports "
          "is set but mem_tile is not 'input_ports'. Set "
          "\"mem_tile\": \"input_ports\" under control_instrumentation to enable L2-L2.");
    }

    if (iniPortsSet && !iniL2L2Enabled && !memTileUsesBlob) {
      xrt_core::message::send(severity_level::error, "XRT",
          "AIE dtrace: AIE_dtrace_settings.memory_tile_input_ports is set but "
          "tile_based_memory_tile_metrics does not include 'input_ports'. Add "
          "tile_based_memory_tile_metrics=all:input_ports (or equivalent) to enable L2-L2.");
    }

    if (l2L2TransferEnabled) {
      const std::string portsStr = profiling_runtime_config::resolveMemoryTileInputPorts();
      const auto designPoints = aie::dtrace::parseL2L2DesignPoints(portsStr);
      if (designPoints.empty()) {
        if (portsStr.empty()) {
          if (l2L2FromBlob) {
            xrt_core::message::send(severity_level::error, "XRT",
                "AIE dtrace: profiling_runtime_config.control_instrumentation.mem_tile is "
                "'input_ports' but memory_tile_input_ports is missing or empty. Add design points "
                "as a {column,row:port} list under control_instrumentation "
                "(e.g. \"memory_tile_input_ports\": \"{1,1:2},{5,1:1},{5,1:2}\"). "
                "L2-L2 counters will not be appended to the CT.");
          } else {
            xrt_core::message::send(severity_level::error, "XRT",
                "AIE dtrace: AIE_dtrace_settings.tile_based_memory_tile_metrics includes "
                "'input_ports' but memory_tile_input_ports is missing or empty. Add design points "
                "as a {column,row:port} list in xrt.ini "
                "(e.g. memory_tile_input_ports={1,1:2},{5,1:1},{5,1:2}). "
                "L2-L2 counters will not be appended to the CT.");
          }
        } else {
          xrt_core::message::send(severity_level::warning, "XRT",
              "AIE dtrace: L2-L2 is enabled but memory_tile_input_ports is invalid "
              "(expected {column,row:port} entries; column is partition-relative, "
              "0 = partition start). "
              "L2-L2 counters will not be appended to the CT.");
        }
        l2L2TransferEnabled = false;
      }
    }

    // Built last so it sees the final l2L2TransferEnabled, which the design
    // point validation above can still turn back off.
    if (useProfileRuns) {
      metricSelections.reserve(runs.size());
      for (size_t i = 0; i < runs.size(); ++i)
        metricSelections.push_back(buildSelectionFromProfileRun(runs[i], i));

      std::stringstream msg;
      msg << "AIE dtrace: profiling " << metricSelections.size()
          << " inferences starting at inference " << startInference << ":";
      for (size_t i = 0; i < metricSelections.size(); ++i)
        msg << "\n  inference " << (startInference + i) << ": "
            << metricSelections[i].describe();
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
    }
    else {
      metricSelections.push_back(buildSelectionFromConfigMetrics());
    }

    xrt_core::message::send(severity_level::info, "XRT", "Finished parsing AIE dtrace metadata.");
  }

  std::string MetricSelection::describe() const
  {
    std::stringstream msg;
    const char* sep = "";

    if (includeBandwidth) {
      msg << "interface_tile=" << bandwidthMetricSet
          << ":" << static_cast<int>(bandwidthChannel);
      sep = ", ";
    }
    if (!coreMetricSet.empty()) {
      msg << sep << "aie_tile=" << coreMetricSet;
      sep = ", ";
    }
    if (includeL2L2)
      msg << sep << "mem_tile=" << INPUT_PORTS_METRIC_SET;

    const std::string out = msg.str();
    return out.empty() ? std::string("no metrics") : out;
  }

  // Reduce the whole-context config maps to the one selection that every
  // inference shares. This is the single-configuration form: the CT is the same
  // no matter how many times the kernel runs.
  MetricSelection AieDtraceMetadata::buildSelectionFromConfigMetrics()
  {
    MetricSelection selection;

    for (const auto& tc : getConfigMetricsVec(CORE_MODULE_IDX)) {
      selection.coreMetricSet = tc.second;
      break;
    }

    // Interface-tile bandwidth metrics are configured by default unless the user
    // turned interface tiles off, which leaves the shim config map empty.
    const auto shimConfigMetrics = getConfigMetricsVec(SHIM_MODULE_IDX);
    selection.includeBandwidth = !shimConfigMetrics.empty();

    if (selection.includeBandwidth) {
      selection.bandwidthMetricSet = shimConfigMetrics.front().second;
      // The detailed_ddr_*_bandwidth metric sets carry a DMA channel in their
      // ":<channel>" suffix, which getConfigMetricsForInterfaceTiles stored in
      // configChannel0 keyed by tile.
      const auto& shimTile = shimConfigMetrics.front().first;
      for (const auto& tc : configChannel0) {
        if ((tc.first.col == shimTile.col) && (tc.first.row == shimTile.row)) {
          selection.bandwidthChannel = tc.second;
          break;
        }
      }
    }

    selection.includeL2L2 = l2L2TransferEnabled;
    return selection;
  }

  // Resolve one profile_runs entry. Unlike the single-configuration form these
  // are not routed through getConfigMetricsFor*Tiles: the CT writer derives its
  // own tiles, so all that is needed here is the metric set names and the DMA
  // channel carried in the interface tile's ":<channel>" suffix.
  MetricSelection
  AieDtraceMetadata::buildSelectionFromProfileRun(
      const profiling_runtime_config::profile_run_t& run, size_t index) const
  {
    const std::string scope = "profile_runs[" + std::to_string(index) + "]";
    MetricSelection selection;

    if (run.interface_tile.has_value() && !run.interface_tile->empty()) {
      std::vector<std::string> parts;
      boost::split(parts, *run.interface_tile, boost::is_any_of(":"));
      const std::string& metricSet = parts.front();

      if (!isBandwidthMetricSet(metricSet)) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "AIE dtrace: " + scope + ".interface_tile='" + *run.interface_tile
            + "' is not a known interface tile metric set; no bandwidth counters "
              "will be programmed for that inference.");
      }
      else if (metricSet != "off") {
        selection.includeBandwidth = true;
        selection.bandwidthMetricSet = metricSet;

        if (parts.size() > 1) {
          try {
            selection.bandwidthChannel = aie::convertStringToUint8(parts[1]);
          }
          catch (const std::invalid_argument&) {
            xrt_core::message::send(severity_level::warning, "XRT",
                "AIE dtrace: channel '" + parts[1] + "' in " + scope
                + ".interface_tile is not an integer; using channel 0.");
          }
        }
      }
    }

    if (run.aie_tile.has_value() && !run.aie_tile->empty()) {
      // Accept "all:<metric>" as well as a bare "<metric>", matching what
      // getConfigMetricsForAIETiles allows for the single-configuration form.
      std::vector<std::string> parts;
      boost::split(parts, *run.aie_tile, boost::is_any_of(":"));
      const std::string& metricSet = parts.back();

      if (!isCoreMetricSet(metricSet)) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "AIE dtrace: " + scope + ".aie_tile='" + *run.aie_tile
            + "' is not a known core (aie) tile metric set. Supported: compute_io_bound, off.");
      }
      else if (metricSet != "off") {
        selection.coreMetricSet = metricSet;
      }
    }

    selection.includeL2L2 = l2L2TransferEnabled
                         && run.mem_tile.has_value()
                         && *run.mem_tile == INPUT_PORTS_METRIC_SET;

    return selection;
  }

  void AieDtraceMetadata::checkDtraceSettings()
  {
    using boost::property_tree::ptree;
    const std::set<std::string> validSettings {
      "tile_based_interface_tile_metrics",
      "tile_based_aie_metrics",
      "tile_based_memory_tile_metrics",
      "memory_tile_input_ports",
      "configure_aie_hardware",
      "config_one_partition",
    };

    auto tree = xrt_core::config::detail::get_ptree_value("AIE_dtrace_settings");
    if (auto val = tree.get_optional<bool>("config_one_partition"))
      configOnePartition = *val;

    for (ptree::iterator pos = tree.begin(); pos != tree.end(); pos++) {
      if (validSettings.find(pos->first) == validSettings.end()) {
        std::stringstream msg;
        msg << "The setting AIE_dtrace_settings." << pos->first << " is not recognized. "
            << "Please check the spelling and compare to supported list:";
        for (auto it = validSettings.cbegin(); it != validSettings.cend(); it++)
          msg << ((it == validSettings.cbegin()) ? " " : ", ") << *it;
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      }
    }
  }

  std::vector<std::string>
  AieDtraceMetadata::getSettingsVector(std::string settingsString)
  {
    if (settingsString.empty())
      return {};
    std::vector<std::string> settingsVector;
    boost::replace_all(settingsString, " ", "");
    boost::split(settingsVector, settingsString, boost::is_any_of(";"));
    return settingsVector;
  }

  bool AieDtraceMetadata::isBandwidthMetricSet(const std::string& metricSet) const
  {
    return bandwidthMetricSets().count(metricSet) > 0;
  }

  bool AieDtraceMetadata::isCoreMetricSet(const std::string& metricSet) const
  {
    return coreMetricSets().count(metricSet) > 0;
  }

  void AieDtraceMetadata::getConfigMetricsForAIETiles(int moduleIdx,
      const std::vector<std::string>& metricsSettings)
  {
    if (metricsSettings.empty())
      return;

    // These settings only enable/disable the metric; the tiles themselves are
    // fixed, and the CT writer decides which ones the chosen metric set needs.
    std::string metricSet;
    for (const auto& setting : metricsSettings) {
      std::vector<std::string> parts;
      boost::split(parts, setting, boost::is_any_of(":"));
      // Accept "all:<metric>", "<col>:<metric>", or bare "<metric>".
      const std::string& candidate = parts.back();
      if (isCoreMetricSet(candidate)) {
        metricSet = candidate;
        break;
      }
    }

    if (metricSet.empty()) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE dtrace: no valid core (aie) tile metric set found in "
          "tile_based_aie_metrics. Supported: compute_io_bound, off.");
      return;
    }

    if (metricSet == "off")
      return;

    tile_type tile;
    tile.col = CORE_METRIC_COL;
    tile.row = CORE_METRIC_ROW;
    tile.active_core = true;
    configMetrics[moduleIdx][tile] = metricSet;

    xrt_core::message::send(severity_level::info, "XRT",
        "AIE dtrace: enabled core (aie) tile metric set '" + metricSet + "'.");
  }

  void AieDtraceMetadata::getConfigMetricsForInterfaceTiles(int moduleIdx,
      const std::vector<std::string>& metricsSettings)
  {
    if (metricsSettings.empty())
      return;

    std::vector<std::vector<std::string>> metrics(metricsSettings.size());

    // Pass 1: all:<metric>[:<channel0>[:<channel1>]]
    for (size_t i = 0; i < metricsSettings.size(); ++i) {
      boost::split(metrics[i], metricsSettings[i], boost::is_any_of(":"));

      if (metrics[i][0].compare("all") != 0)
        continue;
      if (metrics[i].size() < 2 || !isBandwidthMetricSet(metrics[i][1]))
        continue;

      bool foundChannels = false;
      uint8_t channelId0 = 0;
      uint8_t channelId1 = 1;
      if (metrics[i].size() > 2) {
        try {
          foundChannels = true;
          channelId0 = aie::convertStringToUint8(metrics[i][2]);
          channelId1 = (metrics[i].size() < 4) ? channelId0 : aie::convertStringToUint8(metrics[i][3]);
        }
        catch (std::invalid_argument const&) {
          foundChannels = false;
          xrt_core::message::send(severity_level::warning, "XRT",
              "Channel ID specification in tile_based_interface_tile_metrics "
              "is not an integer and hence ignored.");
        }
      }

      std::vector<tile_type> tiles;
      if (foundChannels)
        tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i][1], channelId0);
      else
        tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i][1]);

      for (auto& t : tiles) {
        auto tileItr = std::find_if(configMetrics[moduleIdx].begin(),
            configMetrics[moduleIdx].end(), compareTileByLocMap(t));

        if (tileItr == configMetrics[moduleIdx].end()) {
          configMetrics[moduleIdx][t] = metrics[i][1];
          configChannel0[t] = channelId0;
          configChannel1[t] = channelId1;
        }
        else {
          xrt_core::message::send(severity_level::warning, "XRT",
              "Tile " + std::to_string(t.col) + "," + std::to_string(t.row)
              + " is already configured with metric set " + configMetrics[moduleIdx][t]
              + ". Ignoring setting for set " + metrics[i][1] + ".");
        }
      }
    }

    // Pass 2: <mincolumn>:<maxcolumn>:<metric>[:<channel0>[:<channel1>]]
    for (size_t i = 0; i < metricsSettings.size(); ++i) {
      if ((metrics[i][0].compare("all") == 0) || (metrics[i].size() < 3))
        continue;

      uint8_t maxCol = 0;
      try {
        maxCol = aie::convertStringToUint8(metrics[i][1]);
      }
      catch (std::invalid_argument const&) {
        continue;
      }

      if (!isBandwidthMetricSet(metrics[i][2]))
        continue;

      uint8_t minCol = 0;
      try {
        minCol = aie::convertStringToUint8(metrics[i][0]);
      }
      catch (std::invalid_argument const&) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "Minimum column specification in tile_based_interface_tile_metrics "
            "is not an integer and hence skipped.");
        continue;
      }

      bool foundChannels = false;
      uint8_t channelId0 = 0;
      uint8_t channelId1 = 1;
      if (metrics[i].size() > 3) {
        try {
          foundChannels = true;
          channelId0 = aie::convertStringToUint8(metrics[i][3]);
          channelId1 = (metrics[i].size() == 4) ? channelId0 : aie::convertStringToUint8(metrics[i][4]);
        }
        catch (std::invalid_argument const&) {
          foundChannels = false;
          xrt_core::message::send(severity_level::warning, "XRT",
              "Channel ID specification in tile_based_interface_tile_metrics "
              "is not an integer and hence ignored.");
        }
      }

      int16_t channelNum = foundChannels ? channelId0 : -1;
      auto tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i][2],
          channelNum, true, minCol, maxCol);

      for (auto& t : tiles) {
        configMetrics[moduleIdx][t] = metrics[i][2];
        configChannel0[t] = channelId0;
        configChannel1[t] = channelId1;
      }
    }

    // Pass 3: <singleColumn>:<metric>[:<channel0>[:<channel1>]]
    for (size_t i = 0; i < metricsSettings.size(); ++i) {
      bool isRangeSpecification = false;
      if (metrics[i].size() >= 3) {
        try {
          (void)aie::convertStringToUint8(metrics[i][0]);
          (void)aie::convertStringToUint8(metrics[i][1]);
          isRangeSpecification = true;
        }
        catch (std::invalid_argument const&) {
          isRangeSpecification = false;
        }
      }

      if (isRangeSpecification || (metrics[i].size() == 4) || (metrics[i].size() < 2)
          || (metrics[i][0].compare("all") == 0))
        continue;
      if (!isBandwidthMetricSet(metrics[i][1]))
        continue;

      uint8_t col = 0;
      try {
        col = aie::convertStringToUint8(metrics[i][1]);
        xrt_core::message::send(severity_level::warning, "XRT",
            "tile_based_interface_tile_metrics: invalid format. Ignored: "
            + metricsSettings[i]);
        continue;
      }
      catch (std::invalid_argument const&) {
        try {
          col = aie::convertStringToUint8(metrics[i][0]);
        }
        catch (std::invalid_argument const&) {
          xrt_core::message::send(severity_level::warning, "XRT",
              "Column specification in tile_based_interface_tile_metrics "
              "is not an integer and hence skipped.");
          continue;
        }

        bool foundChannels = false;
        uint8_t channelId0 = 0;
        uint8_t channelId1 = 1;
        if (metrics[i].size() > 2) {
          try {
            foundChannels = true;
            channelId0 = aie::convertStringToUint8(metrics[i][2]);
            channelId1 = (metrics[i].size() == 3) ? channelId0 : aie::convertStringToUint8(metrics[i][3]);
          }
          catch (std::invalid_argument const&) {
            foundChannels = false;
            xrt_core::message::send(severity_level::warning, "XRT",
                "Channel ID specification in tile_based_interface_tile_metrics "
                "is not an integer and hence ignored.");
          }
        }

        int16_t channelNum = foundChannels ? channelId0 : -1;
        auto tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i][1],
            channelNum, true, col, col);

        for (auto& t : tiles) {
          configMetrics[moduleIdx][t] = metrics[i][1];
          configChannel0[t] = channelId0;
          configChannel1[t] = channelId1;
        }
      }
    }

    const std::string defaultSet = "peak_read_bandwidth";
    bool showWarning = true;
    std::vector<tile_type> offTiles;
    const auto& metricVec = bandwidthMetricSets();

    for (auto& tileMetric : configMetrics[moduleIdx]) {
      if (tileMetric.second.empty() || tileMetric.second.compare("off") == 0) {
        offTiles.push_back(tileMetric.first);
        continue;
      }

      if (metricVec.count(tileMetric.second) == 0) {
        if (showWarning) {
          std::string msg = "Unable to find interface_tile metric set " + tileMetric.second
              + ". Using default of " + defaultSet + ". ";
          xrt_core::message::send(severity_level::warning, "XRT", msg);
          showWarning = false;
        }
        tileMetric.second = defaultSet;
      }
    }

    for (auto& t : offTiles)
      configMetrics[moduleIdx].erase(t);
  }

  std::vector<std::pair<tile_type, std::string>>
  AieDtraceMetadata::getConfigMetricsVec(int module)
  {
    if (module < 0 || module >= static_cast<int>(configMetrics.size()))
      return {};
    return {configMetrics[module].begin(), configMetrics[module].end()};
  }

  aie::driver_config
  AieDtraceMetadata::getAIEConfigMetadata()
  {
    return metadataReader->getDriverConfig();
  }

  std::unique_ptr<const AIEProfileFinalConfig>
  AieDtraceMetadata::createAIEProfileConfig()
  {
    std::map<tile_type, uint32_t> emptyBytes;
    std::map<tileKey, LatencyConfig> emptyLatency;
    return std::make_unique<const AIEProfileFinalConfig>(
        configMetrics, configChannel0, configChannel1,
        metadataReader->getAIETileRowOffset(), emptyBytes, emptyLatency);
  }

} // namespace xdp
