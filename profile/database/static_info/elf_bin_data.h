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
#include "xdp/profile/database/static_info/xclbin_types.h"

namespace xdp::aie {
  // Forward declaration; full type is in
  //   xdp/profile/database/static_info/filetypes/base_filetype_impl.h
  // which is pulled in only by elf_bin_data.cpp to keep this header light.
  class BaseFiletypeImpl;
} // namespace xdp::aie

namespace xdp {

  // ElfBinData is the ELF-backed implementation of VPBinData. An application
  // running the Full ELF flow loads a single xrt::elf containing AIE control
  // code + AIE metadata, and no xclbin. The static database represents that
  // device configuration as a CONFIG_ELF_AIE_ONLY ConfigInfo holding a single
  // ElfBinData.
  //
  // AIE state (clock rate, isAIEcounterRead flag, AIECounter list, etc.) is
  // stored on the inherited AIEInfo aggregate, exactly as XclbinBinData does
  // for the xclbin path. This lets writers and database lookups speak through
  // the VPBinData interface uniformly across both binary sources, with no
  // ELF-specific early-return branches.
  //
  // ELF carries no PL data by construction. Calling getPl() on an ElfBinData
  // is a programming error: it throws std::logic_error so the mistake surfaces
  // in test rather than silently returning a permanently-invalid PLInfo&. All
  // happy-path call sites either guard with isXclbin()/source() == XCLBIN, or
  // go through ConfigInfo::getPlBinary() which never returns an ELF binary.
  class ElfBinData final : public VPBinData
  {
  public:
    XDP_CORE_EXPORT
    ElfBinData(xrt::elf elf, std::shared_ptr<xrt_core::device> device);
    ~ElfBinData() override = default;

    // Non-copyable, non-movable: ElfBinData is a unique-resource owner
    // (xrt::elf + shared device handle + cached AIEInfo). Its lifecycle
    // is "construct via make_unique, transfer via raw pointer to
    // ConfigInfo, deleted polymorphically through VPBinData*"; copying
    // would silently duplicate state and bump shared-device refcounts.
    // Move operations are already implicitly suppressed by the
    // user-declared destructor above.
    ElfBinData(const ElfBinData&)            = delete;
    ElfBinData& operator=(const ElfBinData&) = delete;

    // VPBinData interface ------------------------------------------------
    const xrt_core::uuid& getUuid() const override { return m_uuid; }
    const std::string&    getName() const override { return m_name; }
    BinaryInfoType        getType() const override { return m_type; }
    BinDataSource         source()  const override { return BinDataSource::ELF; }

    // VPBinData identity setters. setType is exposed for interface
    // symmetry; the ELF flow currently uses only ELF_AIE_ONLY, which
    // is the default-initialized value of m_type.
    void setUuid(const xrt_core::uuid& value) override { m_uuid = value; }
    void setName(const std::string& value)    override { m_name = value; }
    void setType(BinaryInfoType value)        override { m_type = value; }

    PLInfo&       getPl()       override;
    const PLInfo& getPl() const override;

    AIEInfo&       getAie()       override { return m_aie; }
    const AIEInfo& getAie() const override { return m_aie; }

    // ELF binaries are self-complete (AIE-only by construction, never
    //  paired with a PL piece). buildConfig produces a single-binary
    //  CONFIG_ELF_AIE_ONLY ConfigInfo and ignores devInfo entirely; the
    //  partial-load merge that XclbinBinData performs does not apply.
    std::unique_ptr<ConfigInfo>
    buildConfig(DeviceInfo& devInfo) override;

    // ELF-specific surface ----------------------------------------------
    // Acquire AIE metadata. Tries the AIE_TRACE_METADATA custom section
    // embedded in the ELF first, then falls back to disk-JSON
    // (aie_trace_config.json) matching today's 2-arg ELF flow. Returns
    // the produced filetype reader so the caller can register it on the
    // database's metadata-reader side map; nullptr if neither source is
    // available.
    XDP_CORE_EXPORT
    std::unique_ptr<aie::BaseFiletypeImpl>
    readAIEMetadata(boost::property_tree::ptree& out);

    // Cache the AIE state derivable from a metadata reader on m_aie so
    // subsequent database lookups (clock rate, hw gen) can answer without
    // re-parsing.
    XDP_CORE_EXPORT
    void populateFromReader(const aie::BaseFiletypeImpl& reader);

  private:
    xrt::elf m_elf;
    std::shared_ptr<xrt_core::device> m_device;

    xrt_core::uuid m_uuid;
    std::string    m_name = "elf";
    BinaryInfoType m_type = ELF_AIE_ONLY;

    // The AIE side of the inherited VPBinData state. valid=true is set
    // by the constructor body so writers/database can rely on the same
    // "valid PLInfo or valid AIEInfo per binary" invariant XclbinBinData
    // honors. AIEInfo is defined in pl_aie_info.h (included above).
    AIEInfo m_aie;
  };

} // end namespace xdp

#endif
