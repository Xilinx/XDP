/**
 * Copyright (C) 2021 Xilinx, Inc
 * Copyright (C) 2022-2024 Advanced Micro Devices, Inc. - All rights reserved
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

#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/pl_constructs.h"
#include "xdp/profile/database/static_info/xclbin_info.h"
#include "xdp/profile/database/static_info/device_info.h"
#include "xdp/profile/device/pl_device_intf.h"
#include "core/common/message.h"

namespace xdp {

  PLInfo& PLInfo::operator=(const PLInfo& src)
  {
    // Check for self assignment
    if (this == &src)
      return *this;

    // Release existing PLInfo resources
    releaseResources() ;

    this->hostMaxReadBW    = src.hostMaxReadBW ;
    this->hostMaxWriteBW   = src.hostMaxWriteBW ;
    this->kernelMaxReadBW  = src.kernelMaxReadBW ;
    this->kernelMaxWriteBW = src.kernelMaxWriteBW ;
    this->clockRatePLMHz = src.clockRatePLMHz ;

    this->usesTs2mm = src.usesTs2mm ;
    this->usesFifo = src.usesFifo ;
    this->hasFloatingAIMWithTrace = src.hasFloatingAIMWithTrace ;
    this->hasFloatingASMWithTrace = src.hasFloatingASMWithTrace ;
    this->hasMemoryAIM = src.hasMemoryAIM ;

    for (auto &cu : src.cus)
      this->cus[cu.first] = new ComputeUnitInstance(*cu.second) ;

    for (auto &mi : src.memoryInfo)
      this->memoryInfo[mi.first] = new Memory(*mi.second) ;
    
    this->ams.reserve(src.ams.size()) ;
    for (auto& am : src.ams)
      this->ams.push_back(new Monitor(*am)) ;

    this->aims.reserve(src.aims.size()) ;
    for (auto& aim : src.aims)
      this->aims.push_back(new Monitor(*aim)) ;

    this->asms.reserve(src.asms.size()) ;
    for (auto& asmPtr : src.asms)
      this->asms.push_back(new Monitor(*asmPtr)) ;

    return *this ;
  }

  PLInfo::~PLInfo()
  {
    releaseResources();
  }

  void PLInfo::releaseResources()
  {
    for (auto& i : cus)
      delete i.second ;
    cus.clear();

    for (auto& i : memoryInfo)
      delete i.second ;
    memoryInfo.clear();

    for (auto i : ams)
      delete i ;
    ams.clear();

    for (auto i : aims)
      delete i ;
    aims.clear();

    for (auto i : asms)
      delete i ;
    asms.clear();
  }

  std::vector<ComputeUnitInstance*>
  PLInfo::collectCUs(const std::string& kernelName)
  {
    std::vector<ComputeUnitInstance*> collected;

    for (auto& iter : cus) {
      auto instance = iter.second;
      if (instance->getKernelName() == kernelName)
        collected.push_back(instance);
    }
    return collected;
  }

  void PLInfo::addComputeUnitPorts(const std::string& kernelName,
                                   const std::string& portName,
                                   int32_t portWidth)
  {
    for (const auto& iter : cus) {
      auto cu = iter.second;
      if (cu->getKernelName() == kernelName)
        cu->addPort(portName, portWidth);
    }
  }

  void PLInfo::addArgToPort(const std::string& kernelName,
                            const std::string& argName,
                            const std::string& portName)
  {
    for (const auto& iter : cus) {
      auto cu = iter.second;
      if (cu->getKernelName() == kernelName)
        cu->addArgToPort(argName, portName);
    }
  }

  void PLInfo::connectArgToMemory(const std::string& cuName,
                                  const std::string& portName,
                                  const std::string& argName,
                                  int32_t memId)
  {
    if (memoryInfo.find(memId) == memoryInfo.end())
      return;

    Memory* mem = memoryInfo[memId];
    for (const auto& iter : cus) {
      auto cu = iter.second;
      if (cu->getName() == cuName)
        cu->connectArgToMemory(portName, argName, mem);
    }
  }

  AIEInfo& AIEInfo::operator=(const AIEInfo& src)
  {
    // Check for self assignment
    if (this == &src)
      return *this ;

    // Release existing PLInfo resources
    releaseResources() ;

    this->clockRateAIEMHz = src.clockRateAIEMHz ;
    this->numTracePLIO = src.numTracePLIO ;
    this->isGMIORead = src.isGMIORead ;
    this->isAIEcounterRead = src.isAIEcounterRead ;

    for (auto &aie : src.aieList)
      this->aieList.push_back(new AIECounter(*aie)) ;

    for (auto &gmio : src.gmioList)
      this->gmioList.push_back(new TraceGMIO(*gmio)) ;

    this->aieCoreCountersMap    = src.aieCoreCountersMap ;
    this->aieShimCountersMap    = src.aieShimCountersMap ;
    this->aieMemTileCountersMap = src.aieMemTileCountersMap ;
    this->aieCoreEventsMap      = src.aieCoreEventsMap ;
    this->aieMemoryEventsMap    = src.aieMemoryEventsMap ;
    this->aieShimEventsMap      = src.aieShimEventsMap ;
    this->aieMemTileEventsMap   = src.aieMemTileEventsMap ;

    for (auto &tile : src.aieCfgList)
      this->aieCfgList.push_back(std::make_unique<aie_cfg_tile>(*tile)) ;

    for (auto &noc : src.nocList)
      this->nocList.push_back(new NoCNode(*noc)) ;

    return *this ;
  }

  AIEInfo::~AIEInfo()
  {
    releaseResources();
  }

  void AIEInfo::releaseResources()
  {
    for (auto i : aieList)
      delete i ;
    aieList.clear();

    for (auto i: gmioList)
      delete i;
    gmioList.clear();

    for (auto i : nocList)
      delete i;

    // release aie_cfg_tile unique pointers
    aieCfgList.clear() ;
  }

  XclbinBinData::XclbinBinData(XclbinInfoType xclbinType) : type(xclbinType)
  {
      if (xclbinType == XclbinInfoType::XCLBIN_PL_ONLY) {
        pl.valid  = true;
        aie.valid = false;
      } else if (xclbinType == XclbinInfoType::XCLBIN_AIE_ONLY) {
        pl.valid  = false;
        aie.valid = true;
      } else if (xclbinType == XclbinInfoType::ELF_AIE_ONLY) {
        // Forward-compat: ELF-sourced configuration is AIE-only. The
        // concrete xclbin-backed object is not normally constructed with
        // this enum value (it is reserved for the future ElfBinData
        // implementation), but keep behavior symmetric should it occur.
        pl.valid  = false;
        aie.valid = true;
      }
  }

  ConfigInfo::ConfigInfo(VPBinData* binary) : type(CONFIG_AIE_PL)
  {
    if (binary) {
      switch (binary->getType()) {
        case XCLBIN_AIE_PL:   type = CONFIG_AIE_PL;       break;
        case XCLBIN_PL_ONLY:  type = CONFIG_PL_ONLY;      break;
        case XCLBIN_AIE_ONLY: type = CONFIG_AIE_ONLY;     break;
        case ELF_AIE_ONLY:    type = CONFIG_ELF_AIE_ONLY; break;
        default: break;
      }
      currentBinaries.push_back(binary);
    }
  }

  ConfigInfo::~ConfigInfo()
  {
    for (auto bin : currentBinaries)
      delete bin;
    currentBinaries.clear();

    if (plDeviceIntf) {
      delete plDeviceIntf;
      plDeviceIntf = nullptr;
    }
  }

  xrt_core::uuid ConfigInfo::getConfigUuid()
  {
    if (currentBinaries.size()==1)
      return currentBinaries.back()->getUuid();

    std::string mix_uuid_str;
    for (auto bin : currentBinaries)
      mix_uuid_str += bin->getUuid().to_string();

    return xrt_core::uuid(mix_uuid_str);
  }

  void ConfigInfo::addBinary(VPBinData* newBinary)
  {
    currentBinaries.push_back(newBinary);
  }

  bool ConfigInfo::containsBinary(xrt_core::uuid& uuid)
  {
    for (auto bin : currentBinaries)
    {
      if (bin->getUuid() == uuid)
        return true;
    }

    return false;
  }

  bool ConfigInfo::containsBinaryType(XclbinInfoType& binaryQueryType)
  {
    for (auto bin : currentBinaries)
    {
      if (bin->getType() == binaryQueryType)
        return true;
    }

    return false;
  }

  VPBinData* ConfigInfo::getPlBinary()
  {
    for (auto bin : currentBinaries)
    {
      // ELF binaries carry no PL data and their getPl() throws by design.
      //  Skip them so getPlBinary() returns nullptr for ELF-only configs
      //  (downstream PL clock / PL info lookups then fall through to their
      //   default-value branches).
      if (!bin->isXclbin())
        continue;
      if (bin->getPl().valid)
        return bin;
    }
    return nullptr;
  }

  VPBinData* ConfigInfo::getAieBinary()
  {
    for (auto bin : currentBinaries)
    {
      if (bin->getAie().valid)
        return bin;
    }
    return nullptr;
  }

  std::string ConfigInfo::getBinaryNames()
  {
    std::string name = "";
    if (!currentBinaries.empty()) {
      name += currentBinaries.front()->getName();
      for (size_t i=1; i<currentBinaries.size(); i++)
        name += ", " + currentBinaries[i]->getName();
    }
    return name ;
  }

  bool ConfigInfo::isAiePlusPl()
  {
    if (type == CONFIG_AIE_PL || type == CONFIG_AIE_PL_FORMED)
      return true;
    return false; 
  }
  
  bool ConfigInfo::isAieOnly()
  {
    return type == CONFIG_AIE_ONLY;
  }

  bool ConfigInfo::isPlOnly()
  {
    return type == CONFIG_PL_ONLY;
  }

  bool ConfigInfo::hasBinary(VPBinData* binary)
  {
    for (auto bin : currentBinaries) {
      if (bin == binary)
        return true;
    }
    return false;
  }

    bool ConfigInfo::hasFloatingAIMWithTrace(VPBinData* binary)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid)
          return bin->getPl().hasFloatingAIMWithTrace ;
      }

      return false ;
    }

    bool ConfigInfo::hasFloatingASMWithTrace(VPBinData* binary)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid)
          return bin->getPl().hasFloatingASMWithTrace ;
      }

      return false ;
    }

    uint64_t ConfigInfo::getNumAM(VPBinData* binary)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid)
          return bin->getPl().ams.size() ;
      }

      return 0;
    }

    uint64_t ConfigInfo::getNumUserAMWithTrace(VPBinData* binary)
    {
      uint64_t num = 0;
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto am : bin->getPl().ams) {
            if (am->traceEnabled)
              ++num ;
          }
        }
      }
      return num ;
    }

    uint64_t ConfigInfo::getNumAIM(VPBinData* binary)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary)
          return bin->getPl().aims.size() ;
      }

      return 0 ;
    }

    uint64_t ConfigInfo::getNumUserAIM(VPBinData* binary)
    {
      uint64_t num = 0;
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto aim : bin->getPl().aims) {
            if (!aim->isShellMonitor())
              ++num ;
          }
        }
      }
      return num ;
    }

    uint64_t ConfigInfo::getNumUserAIMWithTrace(VPBinData* binary) const
    {
      uint64_t num = 0;
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto aim : bin->getPl().aims) {
            if (aim->traceEnabled && !aim->isShellMonitor())
              ++num ;
          }
        }
      }

      return num ;
    }

    uint64_t ConfigInfo::getNumASM(VPBinData* binary) const
    {
      for (auto bin : currentBinaries) {
        if (bin == binary)
          return bin->getPl().asms.size() ;
      }
      return 0 ;
    }

    uint64_t ConfigInfo::getNumUserASM(VPBinData* binary) const
    {
      uint64_t num = 0;
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto mon : bin->getPl().asms) {
            if (!mon->isShellMonitor())
              ++num;
          }
        }
      }
      return num ;
    }

    uint64_t ConfigInfo::getNumUserASMWithTrace(VPBinData* binary)
    {
      uint64_t num = 0;
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto mon : bin->getPl().asms) {
            if (mon->traceEnabled && !mon->isShellMonitor())
              ++num;
          }
        }
      }
      return num ;
    }

    uint64_t ConfigInfo::getNumNOC(VPBinData* binary)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary)
          return bin->getAie().nocList.size() ;
      }
      return 0 ;
    }

    Monitor* ConfigInfo::getAMonitor(VPBinData* binary, uint64_t slotId)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto am : bin->getPl().ams) {
            if (am->slotIndex == slotId)
              return am ;
          }
        }
      }
      return nullptr ;
    }

    Monitor* ConfigInfo::getAIMonitor(VPBinData* binary, uint64_t slotId)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto aim : bin->getPl().aims) {
            if (aim->slotIndex == slotId)
              return aim ;
          }
        }
      }
      return nullptr ;
    }

    Monitor* ConfigInfo::getASMonitor(VPBinData* binary, uint64_t slotId)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto streamMonitor : bin->getPl().asms) {
            if (streamMonitor->slotIndex == slotId)
              return streamMonitor ;
          }
        }
      }
      return nullptr ;
    }

    NoCNode* ConfigInfo::getNOC(VPBinData* binary, uint64_t idx)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getAie().valid) {
          if (bin->getAie().nocList.size() <= idx)
            return nullptr;
          return bin->getAie().nocList[idx] ;
        }
      }
      return nullptr ;
    }

    std::vector<Monitor*>* ConfigInfo::getAIMonitors(VPBinData* binary)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary)
          return &(bin->getPl().aims) ;
      }
      return nullptr ;
    }

    std::vector<Monitor*>* ConfigInfo::getASMonitors(VPBinData* binary)
    {
      for (auto bin : currentBinaries) {
        if (bin == binary)
          return &(bin->getPl().asms) ;
      }
      return nullptr ;
    }

    std::vector<Monitor*> ConfigInfo::getUserAIMsWithTrace(VPBinData* binary)
    {
      std::vector<Monitor*> constructed ;
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto aim : bin->getPl().aims) {
            if (aim->traceEnabled && !aim->isShellMonitor())
              constructed.push_back(aim) ;
          }
        }
      }
      return constructed ;
    }

    std::vector<Monitor*> ConfigInfo::getUserASMsWithTrace(VPBinData* binary)
    {
      std::vector<Monitor*> constructed ;
      for (auto bin : currentBinaries) {
        if (bin == binary && bin->getPl().valid) {
          for (auto mon : bin->getPl().asms) {
            if (mon->traceEnabled && !mon->isShellMonitor())
              constructed.push_back(mon) ;
          }
        }
      }
      return constructed ;
    }

    void ConfigInfo::addTraceGMIO(uint32_t id, uint8_t col, uint8_t num,
                                uint8_t stream, uint8_t len, uint16_t bdId)
    {
      for (auto bin : currentBinaries)
      {
        if (bin->getAie().valid)
        {
          xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
                                  "Added GMIO trace of ID "+ std::to_string(id) + ".");
          bin->getAie().gmioList.push_back(new TraceGMIO(id, col, num, stream, len, bdId)) ;
          return ;
        }
      }
    }

    void ConfigInfo::addAIECounter(uint32_t i, uint8_t col, uint8_t r,
                                   uint8_t num, uint16_t start, uint16_t end,
                                   uint8_t reset, uint64_t load, double freq,
                                   const std::string& mod,
                                   const std::string& aieName, uint8_t streamId)
    {
      for (auto bin : currentBinaries)
      {
        if (bin->getAie().valid)
        {
          bin->getAie().aieList.push_back(new AIECounter(i, col, r, num, start, end,
                                                         reset, load, freq, mod,
                                                         aieName,streamId)) ;
          return ;
        }
      }
    }

    void ConfigInfo::addAIECounterResources(uint32_t numCounters,
                                            uint32_t numTiles,
                                            uint8_t moduleType)
    {
      VPBinData* binary = nullptr ;
      for (auto bin : currentBinaries)
      {
        if (bin->getAie().valid)
        {
          binary = bin;
          break ;
        }
      }

      if (!binary)
        return;

      switch (moduleType)
      {
        case module_type::core:
          binary->getAie().aieCoreCountersMap[numCounters] = numTiles ;
          break ;
        case module_type::dma:
          binary->getAie().aieMemoryCountersMap[numCounters] = numTiles ;
          break ;
        case module_type::shim:
          binary->getAie().aieShimCountersMap[numCounters] = numTiles ;
          break ;
        default:
          binary->getAie().aieMemTileCountersMap[numCounters] = numTiles ;
          break ;
      }
    }

    void ConfigInfo::addAIECoreEventResources(uint32_t numEvents,
                                              uint32_t numTiles)
    {
      for (auto bin : currentBinaries)
      {
        if (bin->getAie().valid)
        {
          bin->getAie().aieCoreEventsMap[numEvents] = numTiles ;
          break ;
        }
      }
    }

    void ConfigInfo::addAIEMemoryEventResources(uint32_t numEvents,
                                                uint32_t numTiles)
    {
      for (auto bin : currentBinaries)
      {
        if (bin->getAie().valid)
        {
          bin->getAie().aieMemoryEventsMap[numEvents] = numTiles ;
          break ;
        }
      }
    }

    void ConfigInfo::addAIEShimEventResources(uint32_t numEvents,
                                              uint32_t numTiles)
    {
      for (auto bin : currentBinaries)
      {
        if (bin->getAie().valid)
        {
          bin->getAie().aieShimEventsMap[numEvents] = numTiles ;
          break ;
        }
      }
    }

    void ConfigInfo::addAIEMemTileEventResources(uint32_t numEvents,
                                                 uint32_t numTiles)
    {
      for (auto bin : currentBinaries)
      {
        if (bin->getAie().valid)
        {
          bin->getAie().aieMemTileEventsMap[numEvents] = numTiles ;
          break ;
        }
      }

    }

    void ConfigInfo::addAIECfgTile(std::unique_ptr<aie_cfg_tile>&& tile)
    {
      for (auto bin : currentBinaries)
      {
        if (bin->getAie().valid)
        {
          bin->getAie().aieCfgList.push_back(std::move(tile)) ;
          break ;
        }
      }
    }

    void ConfigInfo::cleanCurrentBinaryInfos(XclbinInfoType binaryType)
    {
      if (binaryType == XCLBIN_AIE_ONLY)   {
        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
                "Skipping the current config cleanup for new aie-only xclbin.");
        return;
      }

      for (auto bin : currentBinaries) {
        // Clean up AIE side of the binary
        if (bin->getAie().valid) {

          for (auto i : bin->getAie().aieList)
            delete i ;
          bin->getAie().aieList.clear() ;

          for (auto i : bin->getAie().gmioList)
            delete i ;
          bin->getAie().gmioList.clear() ;

          bin->getAie().valid = false;
        }
      }
    }

    bool ConfigInfo::hasAIMNamed(const std::string& name)
    {
      for (auto bin : currentBinaries) {
        // ELF binaries have no PL data; their getPl() throws by design.
        if (!bin->isXclbin())
          continue;
        for (auto aim : bin->getPl().aims) {
          if (aim->name.find(name) != std::string::npos)
            return true ;
        }
      }
      return false ;
    }

    // ----------------------------------------------------------------------
    //  XclbinBinData::buildConfig and fromLastConfig
    //
    //  Together these own the xclbin-flow specifics that previously lived
    //  inside DeviceInfo: the partial-load merge (AIE_ONLY paired with a
    //  later PL_ONLY xclbin, or vice versa) and the resulting ConfigInfo
    //  shape. DeviceInfo no longer switches on source type; it just calls
    //  binary->buildConfig(*this) polymorphically.
    // ----------------------------------------------------------------------
    std::unique_ptr<ConfigInfo>
    XclbinBinData::buildConfig(DeviceInfo& devInfo)
    {
      auto config = std::make_unique<ConfigInfo>();
      config->addBinary(this);

      auto currentBinaryType = getType();

      // A complete xclbin (AIE+PL) needs no merge; ConfigInfo defaults to
      //  CONFIG_AIE_PL.
      if (currentBinaryType == XCLBIN_AIE_PL)
        return config;

      // Partial xclbin: search devInfo's last config for the missing
      //  half. Mark this binary's missing side invalid, exactly as
      //  DeviceInfo::createConfig used to.
      VPBinData* missingBinary = nullptr;
      if (currentBinaryType == XCLBIN_AIE_ONLY) {
        getPl().valid = false;
        missingBinary = XclbinBinData::fromLastConfig(devInfo, XCLBIN_PL_ONLY);
      }
      else {
        getAie().valid = false;
        missingBinary = XclbinBinData::fromLastConfig(devInfo, XCLBIN_AIE_ONLY);
      }

      if (missingBinary) {
        const auto& history = devInfo.getLoadedConfigs();
        config->currentBinaries.back()->getAie().numTracePLIO =
            history.empty() ? 0
                            : history.back()->currentBinaries.back()->getAie().numTracePLIO;
        config->addBinary(missingBinary);
        config->type = CONFIG_AIE_PL_FORMED;
      }
      else {
        config->type = (currentBinaryType == XCLBIN_AIE_ONLY)
                       ? CONFIG_AIE_ONLY : CONFIG_PL_ONLY;
      }

      return config;
    }

    XclbinBinData*
    XclbinBinData::fromLastConfig(DeviceInfo& devInfo,
                                  XclbinInfoType xclbinQueryType)
    {
      const auto& configs = devInfo.getLoadedConfigs();
      if (configs.empty()) {
        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
                                "Loaded config on device is empty.");
        return nullptr;
      }

      bool binaryAvailable = false;
      auto lastConfigType = configs.back()->type;
      if (lastConfigType == CONFIG_AIE_PL || lastConfigType == CONFIG_AIE_PL_FORMED)
        binaryAvailable = true;

      if (!binaryAvailable) {
        if (configs.back()->containsBinaryType(xclbinQueryType))
          binaryAvailable = true;
      }

      if (!binaryAvailable)
        return nullptr;

      xrt_core::message::send(xrt_core::message::severity_level::info, "XRT",
                              "Missing binary is available in config.");
      ConfigInfo* lastCfg = configs.back().get();
      for (auto& bin : lastCfg->currentBinaries) {
        if (bin->getType() != xclbinQueryType && bin->getType() != XCLBIN_AIE_PL)
          continue;

        // The partial-load pattern only ever pairs xclbin halves; ELF
        //  binaries get filtered out by the type check above (their
        //  getType() is ELF_AIE_ONLY).
        auto* requiredBinary = new XclbinBinData(xclbinQueryType);

        if (xclbinQueryType == XCLBIN_AIE_ONLY) {
          requiredBinary->getAie() = bin->getAie();
          requiredBinary->getPl().valid = false;
        }
        else {
          requiredBinary->getPl() = bin->getPl();
          requiredBinary->getAie().valid = false;
        }
        requiredBinary->setUuid(bin->getUuid());
        requiredBinary->setName(bin->getName());
        return requiredBinary;
      }

      return nullptr;
    }
} // end namespace xdp
