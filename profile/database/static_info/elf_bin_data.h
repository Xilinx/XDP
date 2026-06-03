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
#include "xdp/profile/database/static_info/vp_bin_data.h"
#include "xdp/profile/database/static_info/xclbin_types.h"

namespace xdp {

  // Forward declarations: PLInfo and AIEInfo are defined in xclbin_info.h.
  // This header is included from xclbin_info.h AFTER those types are defined,
  // so the value member m_aie below sees the full AIEInfo definition.
  struct PLInfo;
  struct AIEInfo;

} // namespace xdp

namespace xdp::aie {
  // Forward declaration; full type is in
  //   xdp/profile/database/static_info/filetypes/base_filetype_impl.h
  // which is pulled in only by elf_bin_data.cpp to keep this header light.
  class BaseFiletypeImpl;
} // namespace xdp::aie

// Pull in the full PLInfo / AIEInfo definitions for the value member below.
// This safety include only fires when a translation unit reaches us before
// xclbin_info.h has been processed. The xclbin_info.h header itself includes
// us at the end (after PLInfo/AIEInfo), in which case the guard short-circuits
// here and the AIEInfo definition is already in scope.
#ifndef XCLBIN_INFO_DOT_H
#include "xdp/profile/database/static_info/xclbin_info.h"
#endif

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
    ElfBinData(xrt::elf elf, std::shared_ptr<xrt_core::device> device);
    ~ElfBinData() override = default;

    // VPBinData interface ------------------------------------------------
    const xrt_core::uuid& getUuid() const override { return m_uuid; }
    const std::string&    getName() const override { return m_name; }
    BinaryInfoType        getType() const override { return ELF_AIE_ONLY; }
    BinDataSource         source()  const override { return BinDataSource::ELF; }

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
    std::unique_ptr<aie::BaseFiletypeImpl>
    readAIEMetadata(boost::property_tree::ptree& out);

    // Cache the AIE state derivable from a metadata reader on m_aie so
    // subsequent database lookups (clock rate, hw gen) can answer without
    // re-parsing.
    void populateFromReader(const aie::BaseFiletypeImpl& reader);

  private:
    xrt::elf m_elf;
    std::shared_ptr<xrt_core::device> m_device;

    xrt_core::uuid m_uuid;
    std::string    m_name = "elf";

    // The AIE side of the inherited VPBinData state. valid=true is set
    // by the constructor body so writers/database can rely on the same
    // "valid PLInfo or valid AIEInfo per binary" invariant XclbinBinData
    // honors. AIEInfo is defined in xclbin_info.h (included above).
    AIEInfo m_aie;
  };

} // end namespace xdp

#endif
