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

#include <string>

#include "core/common/system.h"

#include "xdp/config.h"
#include "xdp/profile/database/static_info/vp_bin_data.h"
#include "xdp/profile/database/static_info/xclbin_types.h"

namespace xdp {

  // Forward declarations: PLInfo and AIEInfo are defined in xclbin_info.h.
  // This header is included from xclbin_info.h AFTER those types are defined,
  // so this skeleton can hold them by value below without circular issues.
  struct PLInfo;
  struct AIEInfo;

} // namespace xdp

// Pull in the full PLInfo / AIEInfo definitions for the value-member fields
// below. This file is intentionally a leaf - it should not be included by
// anything except xclbin_info.h until Part 2 wires ElfBinData into the
// database.
#ifndef XCLBIN_INFO_DOT_H
#include "xdp/profile/database/static_info/xclbin_info.h"
#endif

namespace xdp {

  // Dummy / skeleton implementation of the ELF-backed VPBinData. The class is
  //  defined so the inheritance shape exists and downstream code (Part 2 -
  //  profile plugin integration) can reference VPBinData polymorphically, but
  //  no ElfBinData instance is constructed in this pass.
  //
  //  PLInfo intentionally invalid (.valid = false) - the ELF path never owns
  //  PL data. AIEInfo will be populated by a future ELF metadata reader.
  class ElfBinData final : public VPBinData
  {
  public:
    ElfBinData()
    {
      pl.valid  = false;
      aie.valid = true;
    }
    ~ElfBinData() override = default;

    // VPBinData interface
    const xrt_core::uuid& getUuid() const override { return uuid; }
    const std::string&    getName() const override { return name; }
    XclbinInfoType        getType() const override { return ELF_AIE_ONLY; }
    BinDataSource         source()  const override { return BinDataSource::ELF; }

    PLInfo&       getPl()        override { return pl; }
    const PLInfo& getPl()  const override { return pl; }

    AIEInfo&       getAie()       override { return aie; }
    const AIEInfo& getAie() const override { return aie; }

  private:
    xrt_core::uuid uuid ;
    std::string    name ;

    // PLInfo is intentionally invalid for the ELF path (no PL data).
    PLInfo  pl ;

    // AIEInfo is populated by a future ELF parser.
    AIEInfo aie ;
  } ;

} // end namespace xdp

#endif
