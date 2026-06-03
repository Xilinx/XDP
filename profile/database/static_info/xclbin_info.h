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

#ifndef XCLBIN_INFO_DOT_H
#define XCLBIN_INFO_DOT_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/common/system.h"

#include "xdp/config.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/pl_aie_info.h"
#include "xdp/profile/database/static_info/vp_bin_data.h"
#include "xdp/profile/database/static_info/xclbin_types.h"

namespace xdp {

  // The class XclbinBinData contains all of the information and
  //  configuration for a single xclbin. Since an application may load many
  //  xclbins, and we need to output summary information on all of the
  //  application at the end of execution, we need some configuration data
  //  for all the xclbins that are encountered. An xclbin can contain
  //  PL-specific information and AIE-specific information.
  //
  //  This class is the xclbin-backed implementation of the neutral
  //  VPBinData interface (see vp_bin_data.h). A future ElfBinData
  //  implementation will provide AIE-only data parsed from ELF metadata.
  class XclbinBinData final : public VPBinData
  {
  public:
    explicit XclbinBinData(BinaryInfoType xclbinType) ;
    ~XclbinBinData() override = default;

    // VPBinData interface
    const xrt_core::uuid& getUuid() const override { return m_uuid; }
    const std::string&    getName() const override { return m_name; }
    BinaryInfoType        getType() const override { return m_type; }
    BinDataSource         source()  const override { return BinDataSource::XCLBIN; }

    PLInfo&       getPl()        override { return m_pl; }
    const PLInfo& getPl()  const override { return m_pl; }

    AIEInfo&       getAie()       override { return m_aie; }
    const AIEInfo& getAie() const override { return m_aie; }

    // Build a CONFIG_AIE_PL / CONFIG_AIE_PL_FORMED / CONFIG_AIE_ONLY /
    //  CONFIG_PL_ONLY ConfigInfo. For partial xclbins (AIE_ONLY or
    //  PL_ONLY), consult the device's last config for a complementary
    //  sibling and merge it in via fromLastConfig().
    std::unique_ptr<ConfigInfo>
    buildConfig(DeviceInfo& devInfo) override;

    // Synthesize a "missing piece" XclbinBinData by deep-copying the
    //  matching half from the device's most-recent config. Returns
    //  nullptr when no compatible binary is available. Always produces
    //  XclbinBinData (never ElfBinData), since the partial-load pattern
    //  is xclbin-only by construction.
    XDP_CORE_EXPORT static XclbinBinData*
    fromLastConfig(DeviceInfo& devInfo, BinaryInfoType xclbinQueryType);

    // Setters (VPBinData interface)
    void setUuid(const xrt_core::uuid& value) override { m_uuid = value; }
    void setName(const std::string& value)    override { m_name = value; }
    void setType(BinaryInfoType value)        override { m_type = value; }

  private:
    xrt_core::uuid m_uuid ;
    std::string    m_name ;
    BinaryInfoType m_type {XCLBIN_AIE_PL} ;

    // The configuration of the PL portion of the design
    PLInfo m_pl ;

    // The configuration of the AIE portion of the design (if applicable)
    AIEInfo m_aie ;
  } ;

  // Compatibility alias kept during the VPBinData migration. Plugin and
  //  writer call sites that still spell the xclbin-typed pointer as
  //  XclbinInfo* continue to compile while they are migrated to VPBinData*
  //  in Part 2. Once every caller speaks VPBinData* / XclbinBinData* this
  //  alias can be removed entirely.
  using XclbinInfo = XclbinBinData;

  // The config struct stores multiple binaries (xclbin and/or ELF). It is
  //  a VPBinData aggregator: it does not care whether each entry is an
  //  XclbinBinData or an ElfBinData, only that it exposes the VPBinData
  //  interface.
  struct ConfigInfo {
    // This defines what kind of binaries are loaded on the device.
    ConfigInfoType type {CONFIG_AIE_PL} ;

    // The currently loaded binaries (xclbin and/or ELF) for the device.
    std::vector<VPBinData*> currentBinaries ;

    // The interface with actually communicating with the device.  This
    //  handles the abstractions necessary for communicating in emulation,
    //  actual hardware, and through different mechanisms.
    PLDeviceIntf* plDeviceIntf = nullptr ;

    ConfigInfo() : type(CONFIG_AIE_PL) {};
    ConfigInfo(VPBinData* binary) ;
    ~ConfigInfo() ;

    xrt_core::uuid getConfigUuid() ;
    void addBinary(VPBinData* newBinary) ;
    inline void updateType(ConfigInfoType cfgType) { type=cfgType; }

    bool containsBinary(xrt_core::uuid& uuid) ;
    bool containsBinaryType(BinaryInfoType& binaryQueryType);

    XDP_CORE_EXPORT VPBinData* getPlBinary() ;
    XDP_CORE_EXPORT VPBinData* getAieBinary() ;
    XDP_CORE_EXPORT std::string getBinaryNames() ;

    bool isAiePlusPl() ;
    bool isAieOnly();
    bool isPlOnly();
    bool hasBinary(VPBinData* binary);

    bool hasFloatingAIMWithTrace(VPBinData* binary);
    bool hasFloatingASMWithTrace(VPBinData* binary);

    uint64_t getNumAM(VPBinData* binary) ;
    uint64_t getNumUserAMWithTrace(VPBinData* binary) ;
    uint64_t getNumAIM(VPBinData* binary) ;
    uint64_t getNumUserAIM(VPBinData* binary) ;
    uint64_t getNumUserAIMWithTrace(VPBinData* binary) const ;

    uint64_t getNumASM(VPBinData* binary) const ;
    uint64_t getNumUserASM(VPBinData* binary) const ;
    uint64_t getNumUserASMWithTrace(VPBinData* binary) ;

    uint64_t getNumNOC(VPBinData* binary) ;
    Monitor* getAMonitor(VPBinData* binary, uint64_t slotId) ;
    Monitor* getAIMonitor(VPBinData* binary, uint64_t slotId) ;
    Monitor* getASMonitor(VPBinData* binary, uint64_t slotId) ;
    NoCNode* getNOC(VPBinData* binary, uint64_t idx) ;
    std::vector<Monitor*>* getAIMonitors(VPBinData* binary) ;
    std::vector<Monitor*>* getASMonitors(VPBinData* binary) ;
    std::vector<Monitor*> getUserAIMsWithTrace(VPBinData* binary) ;
    std::vector<Monitor*> getUserASMsWithTrace(VPBinData* binary) ;

    void addTraceGMIO(uint32_t id, uint8_t col, uint8_t num,
                                uint8_t stream, uint8_t len, uint16_t bdId = UINT16_MAX) ;
    void addAIECounter(uint32_t i, uint8_t col, uint8_t r,
                                 uint8_t num, uint16_t start, uint16_t end,
                                 uint8_t reset, uint64_t load, double freq,
                                 const std::string& mod,
                                 const std::string& aieName, uint8_t streamId=0) ;
    void addAIECounterResources(uint32_t numCounters,
                                            uint32_t numTiles,
                                            uint8_t moduleType) ;
    void addAIECoreEventResources(uint32_t numEvents,
                                            uint32_t numTiles) ;
    void addAIEMemoryEventResources(uint32_t numEvents,
                                            uint32_t numTiles) ;
    void addAIEShimEventResources(uint32_t numEvents,
                                              uint32_t numTiles) ;
    void addAIEMemTileEventResources(uint32_t numEvents,
                                              uint32_t numTiles) ;
    void addAIECfgTile(std::unique_ptr<aie_cfg_tile>&& tile) ;
    void cleanCurrentBinaryInfos(BinaryInfoType binaryType) ;
    bool hasAIMNamed(const std::string& name) ;
  } ;

} // end namespace xdp

#endif
