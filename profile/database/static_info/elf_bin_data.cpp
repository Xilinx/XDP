// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2024-2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_CORE_SOURCE

#include "xdp/profile/database/static_info/elf_bin_data.h"

#include <stdexcept>
#include <utility>

#include "core/common/message.h"

#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/database/static_info/filetypes/base_filetype_impl.h"
#include "xdp/profile/database/static_info/xclbin_info.h"

namespace xdp {

  namespace pt = boost::property_tree;
  using severity_level = xrt_core::message::severity_level;

  ElfBinData::ElfBinData(xrt::elf elf,
                         std::shared_ptr<xrt_core::device> device,
                         xrt_core::uuid uuid)
    : m_elf(std::move(elf))
    , m_device(std::move(device))
    , m_uuid(std::move(uuid))
  {
    m_aie.valid = true;
  }

  PLInfo&
  ElfBinData::getPl()
  {
    throw std::logic_error(
      "xdp::ElfBinData::getPl(): the ELF flow carries no PL data; "
      "callers must guard with VPBinData::isXclbin() or use "
      "ConfigInfo::getPlBinary() which never returns an ELF binary.");
  }

  const PLInfo&
  ElfBinData::getPl() const
  {
    throw std::logic_error(
      "xdp::ElfBinData::getPl() const: the ELF flow carries no PL data; "
      "callers must guard with VPBinData::isXclbin() or use "
      "ConfigInfo::getPlBinary() which never returns an ELF binary.");
  }

  std::unique_ptr<aie::BaseFiletypeImpl>
  ElfBinData::readAIEMetadata(pt::ptree& out)
  {
    // Prefer the AIE_METADATA ELF custom section: the source of truth
    // for the Full ELF flow.
    try {
      auto data = m_elf.get_custom_section("AIE_METADATA");
      if (data.data() && data.size()) {
        auto reader = aie::readAIEMetadata(data.data(), data.size(), out);
        if (reader) {
          xrt_core::message::send(severity_level::debug, "XRT",
            "AIE metadata read from ELF custom section.");
          return reader;
        }
      }
    }
    catch (const std::exception& e) {
      std::string msg = "AIE metadata ELF custom section unavailable: ";
      msg += e.what();
      xrt_core::message::send(severity_level::debug, "XRT", msg);
    }

    // Disk-JSON fallback.
    auto reader = aie::readAIEMetadata("aie_trace_config.json", out);
    if (reader) {
      xrt_core::message::send(severity_level::debug, "XRT",
        "AIE metadata read from disk (aie_trace_config.json) for ELF flow.");
    }
    else {
      xrt_core::message::send(severity_level::debug, "XRT",
        "AIE metadata not available for ELF flow.");
    }
    return reader;
  }

  void
  ElfBinData::populateFromReader(const aie::BaseFiletypeImpl& reader)
  {
    try {
      m_aie.clockRateAIEMHz = reader.getAIEClockFreqMHz();
    }
    catch (const std::exception&) {
      // Keep AIEInfo's default clock rate if the reader cannot report one.
    }
  }

  std::unique_ptr<ConfigInfo>
  ElfBinData::buildConfig(DeviceInfo& /*devInfo*/)
  {
    auto config = std::make_unique<ConfigInfo>();
    config->addBinary(this);
    config->type = CONFIG_ELF_AIE_ONLY;
    return config;
  }

} // namespace xdp
