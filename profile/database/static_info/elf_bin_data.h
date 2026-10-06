/**
 * Copyright (C) 2024-2026 Advanced Micro Devices, Inc. - All rights reserved
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

#ifndef ELF_BIN_DATA_DOT_H
#define ELF_BIN_DATA_DOT_H

#include <memory>
#include <string>

#include <boost/property_tree/ptree.hpp>

#include "core/common/system.h"
#include "core/include/xrt/experimental/xrt_elf.h"

#include "xdp/config.h"
#include "xdp/profile/database/static_info/pl_aie_info.h"
#include "xdp/profile/database/static_info/vp_bin_data.h"
#include "xdp/profile/database/static_info/binary_types.h"

namespace xdp::aie {
  // Forward declaration; full type is in
  //   xdp/profile/database/static_info/filetypes/base_filetype_impl.h
  // which is pulled in only by elf_bin_data.cpp to keep this header light.
  class BaseFiletypeImpl;
} // namespace xdp::aie

namespace xdp {

  // ElfBinData is the ELF-backed implementation of VPBinData for the Full ELF
  // flow: a single xrt::elf carrying AIE control code + AIE metadata, no
  // xclbin. AIE state is stored on the inherited AIEInfo aggregate, so writers
  // and database lookups speak through VPBinData uniformly for both sources.
  //
  // ELF carries no PL data, so getPl() throws std::logic_error. Callers must
  // guard with isXclbin() or go through ConfigInfo::getPlBinary(), which never
  // returns an ELF binary.
  class ElfBinData final : public VPBinData
  {
  public:
    // The config UUID is supplied by the caller, which has already derived and
    // validated it via xrt::elf::get_cfg_uuid()
    XDP_CORE_EXPORT
    ElfBinData(xrt::elf elf,
               std::shared_ptr<xrt_core::device> device,
               xrt_core::uuid uuid);
    ~ElfBinData() override = default;

    ElfBinData(const ElfBinData&)            = delete;
    ElfBinData& operator=(const ElfBinData&) = delete;

    // VPBinData interface ------------------------------------------------
    const xrt_core::uuid& getUuid() const override { return m_uuid; }
    const std::string&    getName() const override { return m_name; }
    BinaryInfoType        getType() const override { return m_type; }
    BinDataSource         source()  const override { return BinDataSource::ELF; }

    // Identity setters
    void setUuid(const xrt_core::uuid& value) override { m_uuid = value; }
    void setName(const std::string& value)    override { m_name = value; }
    void setType(BinaryInfoType value)        override { m_type = value; }

    PLInfo&  getPl()  override;
    AIEInfo& getAie() override { return m_aie; }

    // ELF binaries are self-complete: produces a single-binary
    // CONFIG_ELF_AIE_ONLY ConfigInfo and ignores devInfo.
    std::unique_ptr<ConfigInfo>
    buildConfig(DeviceInfo& devInfo) override;

    // Acquire AIE metadata: tries the AIE_METADATA ELF custom section first,
    // then falls back to disk-JSON (aie_trace_config.json). Returns the
    // filetype reader the caller registers, or nullptr if neither is available.
    XDP_CORE_EXPORT
    std::unique_ptr<aie::BaseFiletypeImpl>
    readAIEMetadata(boost::property_tree::ptree& out);

    // Cache AIE state from a metadata reader onto m_aie for later lookups.
    XDP_CORE_EXPORT
    void populateFromReader(const aie::BaseFiletypeImpl& reader);

  private:
    xrt::elf m_elf;
    std::shared_ptr<xrt_core::device> m_device;

    xrt_core::uuid m_uuid;
    std::string    m_name = "elf";
    BinaryInfoType m_type = ELF_AIE_ONLY;

    AIEInfo m_aie;
  };

} // end namespace xdp

#endif
