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

#ifndef VP_BIN_DATA_DOT_H
#define VP_BIN_DATA_DOT_H

#include <string>

#include "core/common/system.h"

#include "xdp/config.h"
#include "xdp/profile/database/static_info/xclbin_types.h"

namespace xdp {

  // Forward declarations of the PL/AIE info aggregates owned by concrete
  // VPBinData implementations. Defined in xclbin_info.h.
  struct PLInfo;
  struct AIEInfo;

  // VPBinData is the neutral abstract interface for any "binary data"
  // associated with a profiling configuration on a device. Today the only
  // backing implementation is XclbinBinData (xclbin source). A future
  // ElfBinData implementation will provide AIE-only data parsed from
  // ELF metadata.
  //
  // Both PLInfo and AIEInfo are exposed on the interface for symmetry; an
  // ELF-backed implementation may simply leave PLInfo invalid/empty.
  class VPBinData
  {
  public:
    virtual ~VPBinData() = default;

    virtual const xrt_core::uuid& getUuid() const = 0;
    virtual const std::string&    getName() const = 0;
    virtual XclbinInfoType        getType() const = 0;
    virtual BinDataSource         source()  const = 0;

    bool isXclbin() const { return source() == BinDataSource::XCLBIN; }
    bool isElf()    const { return source() == BinDataSource::ELF;    }

    virtual PLInfo&        getPl()       = 0;
    virtual const PLInfo&  getPl() const = 0;

    virtual AIEInfo&       getAie()       = 0;
    virtual const AIEInfo& getAie() const = 0;

  protected:
    VPBinData() = default;
  };

} // end namespace xdp

#endif
