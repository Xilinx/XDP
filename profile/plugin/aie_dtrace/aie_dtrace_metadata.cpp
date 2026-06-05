// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2026 Advanced Micro Devices, Inc. All rights reserved

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
#include "xdp/profile/plugin/vp_base/profiling_runtime_config.h"

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  static const std::set<std::string>& bandwidthMetricSets()
  {
    static const std::set<std::string> metrics = {
      "ddr_bandwidth", "read_bandwidth", "write_bandwidth",
      "peak_read_bandwidth", "peak_write_bandwidth", "off"};
    return metrics;
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

    if (usingBlob) {
      if (ci.aie_tile.has_value() && !ci.aie_tile->empty()) {
        xrt_core::message::send(severity_level::info, "XRT",
            "AIE dtrace: core tile metric '" + *ci.aie_tile
            + "' from profiling_runtime_config will be supported in a follow-up.");
      }
      if (ci.mem_tile.has_value() && !ci.mem_tile->empty()) {
        xrt_core::message::send(severity_level::info, "XRT",
            "AIE dtrace: mem tile metric '" + *ci.mem_tile
            + "' from profiling_runtime_config will be supported in a follow-up.");
      }
    }

    std::vector<std::string> metricsSettings;
    if (usingBlob && ci.interface_tile.has_value() && !ci.interface_tile->empty()) {
      xrt_core::message::send(severity_level::info, "XRT",
          "AIE dtrace: using interface_tile metric '" + *ci.interface_tile
          + "' from Debug.profiling_runtime_config.");
      metricsSettings = getSettingsVector("all:" + *ci.interface_tile);
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

    xrt_core::message::send(severity_level::info, "XRT", "Finished parsing AIE dtrace metadata.");
  }

  void AieDtraceMetadata::checkDtraceSettings()
  {
    using boost::property_tree::ptree;
    const std::set<std::string> validSettings {
      "tile_based_interface_tile_metrics",
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
