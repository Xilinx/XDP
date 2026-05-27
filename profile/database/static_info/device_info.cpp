/**
 * Copyright (C) 2021 Xilinx, Inc
 * Copyright (C) 2022-2024 Advanced Micro Devices, Inc - All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define XDP_CORE_SOURCE

#include <memory>
#include "xdp/profile/database/static_info/device_info.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/pl_constructs.h"
#include "xdp/profile/database/static_info/xclbin_info.h"
#include "xdp/profile/device/pl_device_intf.h"
#include "core/common/message.h"

namespace xdp {

  DeviceInfo::~DeviceInfo()
  {
    loadedConfigInfos.clear() ;
  }

  std::string DeviceInfo::getUniqueDeviceName() const
  {
    return deviceName + "-" + std::to_string(deviceId) ;
  }

  xrt_core::uuid DeviceInfo::currentXclbinUUID()
  {
    if (loadedConfigInfos.size() <= 0)
      return xrt_core::uuid() ;
    return loadedConfigInfos.back()->getConfigUuid();
  }

  VPBinData* DeviceInfo::createXclbinFromLastConfig(XclbinInfoType xclbinQueryType)
  {
    VPBinData* requiredBinary = nullptr;
    if (loadedConfigInfos.empty()) {
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", "Loaded config on device is empty.");
      return requiredBinary;
    }

    bool binaryAvailable = false;
    auto lastConfigType = loadedConfigInfos.back()->type;
    if (lastConfigType == CONFIG_AIE_PL || lastConfigType == CONFIG_AIE_PL_FORMED)
      binaryAvailable = true;

    if (!binaryAvailable) {
      if (loadedConfigInfos.back()->containsBinaryType(xclbinQueryType))
        binaryAvailable = true;
    }

    if (binaryAvailable) {
      xrt_core::message::send(xrt_core::message::severity_level::info, "XRT", "Missing binary is available in config.");
      ConfigInfo* lastCfg = loadedConfigInfos.back().get();
      for (auto &bin : lastCfg->currentBinaries)
      {
        if (bin->getType() == xclbinQueryType || bin->getType() == XCLBIN_AIE_PL) {
          // Construct the missing binary based on the query enum. ELF_AIE_ONLY
          //  yields an ElfBinData; everything else yields an XclbinBinData.
          //  The ELF branch is currently dead code (no caller passes
          //  ELF_AIE_ONLY) but is wired here for the future ELF flow.
          if (xclbinQueryType == ELF_AIE_ONLY) {
            requiredBinary = new ElfBinData();
          } else {
            requiredBinary = new XclbinBinData(xclbinQueryType);
          }

          if (xclbinQueryType == XCLBIN_AIE_ONLY || xclbinQueryType == ELF_AIE_ONLY)
          {
            // Perform deep copy of missing AIE binary
            requiredBinary->getAie() = bin->getAie();
            requiredBinary->getPl().valid = false ;
          }
          else
          {
            // Perform deep copy of missing PL binary
            requiredBinary->getPl() = bin->getPl();
            requiredBinary->getAie().valid = false ;
          }
          // setUuid/setName are XclbinBinData-only setters; route through a
          //  downcast guarded by isXclbin(). ElfBinData currently has no
          //  identity setters and keeps its defaults.
          if (requiredBinary->isXclbin()) {
            auto* xclbinBin = static_cast<XclbinBinData*>(requiredBinary);
            xclbinBin->setUuid(bin->getUuid()) ;
            xclbinBin->setName(bin->getName()) ;
          }
          break;  // Need only one such missing binary from last config.
        }
      }
    }
    return requiredBinary;
  }

  void DeviceInfo::createConfig(VPBinData* binary)
  {
    // Create a new config
    std::unique_ptr<ConfigInfo> config = std::make_unique<ConfigInfo>();
    config->addBinary(binary);

    auto currentBinaryType = binary->getType();

    // Check if this itself is a complete xclbin (AIE+PL).
    if (currentBinaryType == XCLBIN_AIE_PL)
    {
      loadedConfigInfos.push_back(std::move(config));
      return;
    }

    // If it is not a complete xclbin.
    //  Check what is missing & request that missing binary.
    //    a. AIEInfo or PLInfo
    VPBinData *missingBinary = nullptr;
    if (currentBinaryType == XCLBIN_AIE_ONLY)
    {
      binary->getPl().valid = false ;
      missingBinary = createXclbinFromLastConfig(XCLBIN_PL_ONLY);
    }
    else
    {
      binary->getAie().valid = false ;
      missingBinary = createXclbinFromLastConfig(XCLBIN_AIE_ONLY);
    }

    // If missing part of the binary is available.
    if (missingBinary)
    {
      config->currentBinaries.back()->getAie().numTracePLIO = loadedConfigInfos.size() == 0 ? 0 : loadedConfigInfos.back()->currentBinaries.back()->getAie().numTracePLIO;
      config->addBinary(missingBinary);
      config->type = CONFIG_AIE_PL_FORMED;
    }
    else
    {
      // If missing part of the binary is not available.
      // This is same binary type load as previous binary.
      config->type = (currentBinaryType == XCLBIN_AIE_ONLY) ? CONFIG_AIE_ONLY : CONFIG_PL_ONLY ;
    }

    loadedConfigInfos.push_back(std::move(config));
  }

  void DeviceInfo::createEmptyConfig()
  {
    // Create a new empty config
    std::unique_ptr<ConfigInfo> config = std::make_unique<ConfigInfo>();
    loadedConfigInfos.push_back(std::move(config));
  }

  bool DeviceInfo::hasFloatingAIMWithTrace(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->hasFloatingAIMWithTrace(binary);
    }
    return false ;
  }

  bool DeviceInfo::hasFloatingASMWithTrace(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->hasFloatingASMWithTrace(binary);
    }
    return false ;
  }

  uint64_t DeviceInfo::getNumAM(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumAM(binary);
    }
    return 0 ;
  }

  uint64_t DeviceInfo::getNumUserAMWithTrace(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumUserAMWithTrace(binary) ;
    }
    return 0 ;
  }

  uint64_t DeviceInfo::getNumAIM(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumAIM(binary) ;
    }

    return 0 ;
  }

  uint64_t DeviceInfo::getNumUserAIM(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumUserAIM(binary) ;
    }
    return 0 ;
  }

  uint64_t DeviceInfo::getNumUserAIMWithTrace(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumUserAIMWithTrace(binary) ;
    }
    return 0 ;
  }

  uint64_t DeviceInfo::getNumASM(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumASM(binary) ;
    }
    return 0 ;
  }

  uint64_t DeviceInfo::getNumUserASM(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumUserASM(binary) ;
    }
    return 0;
  }

  uint64_t DeviceInfo::getNumUserASMWithTrace(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumUserASMWithTrace(binary) ;
    }
    return 0 ;
  }

  uint64_t DeviceInfo::getNumNOC(VPBinData* binary) const
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNumNOC(binary) ;
    }
    return 0 ;
  }

  Monitor* DeviceInfo::getAMonitor(VPBinData* binary, uint64_t slotId)
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getAMonitor(binary, slotId) ;
    }
    return nullptr ;
  }

  Monitor* DeviceInfo::getAIMonitor(VPBinData* binary, uint64_t slotId)
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getAIMonitor(binary, slotId) ;
    }
    return nullptr ;
  }

  Monitor* DeviceInfo::getASMonitor(VPBinData* binary, uint64_t slotId)
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getASMonitor(binary, slotId) ;
    }
    return nullptr ;
  }

  NoCNode* DeviceInfo::getNOC(VPBinData* binary, uint64_t idx)
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getNOC(binary, idx) ;
    }
    return nullptr ;
  }

  std::vector<Monitor*>* DeviceInfo::getAIMonitors(VPBinData* binary)
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getAIMonitors(binary) ;
    }
    return nullptr ;
  }

  std::vector<Monitor*>* DeviceInfo::getASMonitors(VPBinData* binary)
  {
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getASMonitors(binary) ;
    }
    return nullptr ;
  }

  std::vector<Monitor*> DeviceInfo::getUserAIMsWithTrace(VPBinData* binary)
  {
    std::vector<Monitor*> constructed ;
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getUserAIMsWithTrace(binary) ;
    }
    return constructed ;
  }

  std::vector<Monitor*> DeviceInfo::getUserASMsWithTrace(VPBinData* binary)
  {
    std::vector<Monitor*> constructed ;
    for (const auto& cfg : getLoadedConfigs()) {
      if (cfg->hasBinary(binary))
        return cfg->getUserASMsWithTrace(binary) ;
    }
    return constructed ;
  }

  void DeviceInfo::addTraceGMIO(uint32_t id, uint8_t col, uint8_t num,
                                uint8_t stream, uint8_t len, uint16_t bdId)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->addTraceGMIO(id, col, num, stream, len, bdId) ;
  }

  void DeviceInfo::addAIECounter(uint32_t i, uint8_t col, uint8_t row,
                                 uint8_t num, uint16_t start, uint16_t end,
                                 uint8_t reset, uint64_t load, double freq,
                                 const std::string& mod,
                                 const std::string& aieName, uint8_t streamId)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->addAIECounter(i, col, row, num, start, end,
                          reset, load, freq, mod, aieName, streamId) ;
  }

  void DeviceInfo::addAIECounterResources(uint32_t numCounters,
                                          uint32_t numTiles,
                                          uint8_t moduleType)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->addAIECounterResources(numCounters, numTiles, moduleType) ;
  }

  void DeviceInfo::addAIECoreEventResources(uint32_t numEvents,
                                            uint32_t numTiles)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->addAIECoreEventResources(numEvents, numTiles) ;
  }

  void DeviceInfo::addAIEMemoryEventResources(uint32_t numEvents,
                                              uint32_t numTiles)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->addAIEMemoryEventResources(numEvents, numTiles) ;
  }

  void DeviceInfo::addAIEShimEventResources(uint32_t numEvents,
                                            uint32_t numTiles)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->addAIEShimEventResources(numEvents, numTiles) ;
  }

  void DeviceInfo::addAIEMemTileEventResources(uint32_t numEvents,
                                               uint32_t numTiles)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->addAIEMemTileEventResources(numEvents, numTiles) ;
  }

  void DeviceInfo::addAIECfgTile(std::unique_ptr<aie_cfg_tile>& tile)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->addAIECfgTile(std::move(tile)) ;
  }

  ConfigInfo* DeviceInfo::currentConfig() const
  {
    if (getLoadedConfigs().empty())
      return nullptr ;

    return getLoadedConfigs().back().get() ;
  }

  void DeviceInfo::cleanCurrentConfig(XclbinInfoType type)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return ;

    config->cleanCurrentBinaryInfos(type) ;
  }

  double DeviceInfo::getMaxClockRatePLMHz()
  {
    if (deviceName.find("aws") != std::string::npos)
      return 250.0;
    return 300.0;
  }

  bool DeviceInfo::hasAIMNamed(const std::string& name)
  {
    ConfigInfo* config = currentConfig() ;
    if (!config || config->currentBinaries.empty())
      return false;

    return config->hasAIMNamed(name) ;
  }

  bool DeviceInfo::hasDMAMonitor()
  {
    return hasAIMNamed("Host to Device") ;
  }

  bool DeviceInfo::hasDMABypassMonitor()
  {
    return hasAIMNamed("Peer to Peer") ;
  }

  bool DeviceInfo::hasKDMAMonitor()
  {
    return hasAIMNamed("Memory to Memory") ;
  }

} // end namespace xdp
