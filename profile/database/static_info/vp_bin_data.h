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

#include <memory>
#include <string>

#include "core/common/system.h"

#include "xdp/config.h"
#include "xdp/profile/database/static_info/binary_types.h"

namespace xdp {

  // Forward declarations
  struct PLInfo;
  struct AIEInfo;
  struct ConfigInfo;
  struct DeviceInfo;

  // VPBinData is the neutral abstract interface for any "binary data"
  // associated with a profiling configuration on a device. It is backed by
  // XclbinBinData (xclbin source) and ElfBinData (ELF source). Both PLInfo
  // and AIEInfo are exposed for symmetry; an ELF binary leaves PLInfo empty.
  class VPBinData
  {
  public:
    virtual ~VPBinData() = default;

    virtual const xrt_core::uuid& getUuid() const = 0;
    virtual const std::string&    getName() const = 0;
    virtual BinaryInfoType        getType() const = 0;
    virtual BinDataSource         source()  const = 0;

    // Identity setters
    virtual void setUuid(const xrt_core::uuid& value) = 0;
    virtual void setName(const std::string& value)    = 0;
    virtual void setType(BinaryInfoType value)        = 0;

    bool isXclbin() const { return source() == BinDataSource::XCLBIN; }

    virtual PLInfo&  getPl()  = 0;
    virtual AIEInfo& getAie() = 0;

    // Build a ConfigInfo wrapping this binary. The returned ConfigInfo takes
    // ownership of 'this' and deletes it through VPBinData* in ~ConfigInfo().
    virtual std::unique_ptr<ConfigInfo>
    buildConfig(DeviceInfo& devInfo) = 0;

  protected:
    VPBinData() = default;
  };

} // end namespace xdp

#endif
