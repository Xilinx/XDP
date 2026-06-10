// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ve2.h"
#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ct_writer.h"
#include "xdp/profile/plugin/aie_dtrace/ve2/elf_helper.h"

#include "core/common/api/hw_context_int.h"
#include "core/common/api/kernel_int.h"
#include "core/common/config_reader.h"
#include "core/common/message.h"
#include "core/common/shim/hwctx_handle.h"

#include "xdp/profile/database/static_info/aie_util.h"

#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <sstream>

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  static constexpr int SHIM_MODULE_IDX = static_cast<int>(module_type::shim);

  AieDtrace_VE2Impl::AieDtrace_VE2Impl(VPDatabase* database,
                                         std::shared_ptr<AieDtraceMetadata> metadata,
                                         uint64_t deviceID)
      : AieDtraceImpl(database, metadata, deviceID)
  {}

  void AieDtrace_VE2Impl::updateDevice()
  {
    // Bandwidth CT generation configures hardware via write_reg in the begin block.
  }

  void AieDtrace_VE2Impl::computeOpLocations(void* elf_handle, const std::string& kernel_name)
  {
    if (m_op_locations_cache.count(kernel_name))
      return;

    if (!elf_handle) {
      xrt_core::message::send(severity_level::debug, "XRT",
          "AIE dtrace: No ELF handle available for kernel '" + kernel_name + "'");
      return;
    }

    try {
      auto buf = xdp::get_elf_buffer(elf_handle);
      aiebu::aiebu_assembler assembler(buf);

      auto get_op_tbl = [&]() {
        if (!kernel_name.empty()) {
          try {
            return assembler.get_op_locations(0x1c, kernel_name);
          }
          catch (...) {
            xrt_core::message::send(severity_level::debug, "XRT",
                "AIE dtrace: get_op_locations with kernel name '" + kernel_name
                + "' failed, retrying without kernel name");
          }
        }
        return assembler.get_op_locations(0x1c);
      };

      m_op_locations_cache[kernel_name] = get_op_tbl().get_line_info();

      std::stringstream msg;
      msg << "AIE dtrace: Extracted " << m_op_locations_cache[kernel_name].size()
          << " instance op_locations for kernel '" << kernel_name << "' from ELF";
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }
    catch (const std::exception& e) {
      std::stringstream msg;
      msg << "AIE dtrace: Could not extract op_locations from ELF for kernel '"
          << kernel_name << "': " << e.what();
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }
  }

  void AieDtrace_VE2Impl::generateCTForRun(void* run_impl_ptr, void* hwctx, uint32_t run_uid,
                                             const std::string& kernel_name,
                                             void* elf_handle)
  {
    if (!xrt_core::config::get_aie_dtrace())
      return;

    auto ctx = xrt_core::hw_context_int::create_hw_context_from_implementation(hwctx);
    auto slotIdx = static_cast<xrt_core::hwctx_handle*>(ctx)->get_slotidx();

    std::string filename = "aie_dtrace_ctx_" + std::to_string(slotIdx)
                         + "_run_" + std::to_string(run_uid) + ".ct";
    std::string outputPath = (std::filesystem::current_path() / filename).string();

    computeOpLocations(elf_handle, kernel_name);

    auto it = m_op_locations_cache.find(kernel_name);
    if (it == m_op_locations_cache.end() || it->second.empty()) {
      xrt_core::message::send(severity_level::debug, "XRT",
          "AIE dtrace: No op_locations for kernel '" + kernel_name + "'; skipping CT generation.");
      return;
    }

    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(hwctx);
    uint8_t partitionStartCol = aiePartitionPt.empty() ? 0
        : static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("start_col"));

    AieDtraceCTWriter ctWriter(db, metadata, deviceID, partitionStartCol);

    std::string bandwidthMetricSet = "peak_read_bandwidth";
    uint8_t bandwidthChannel = 0;
    auto shimConfigMetrics = metadata->getConfigMetricsVec(SHIM_MODULE_IDX);
    if (!shimConfigMetrics.empty()) {
      bandwidthMetricSet = shimConfigMetrics.front().second;
      // The detailed_ddr_*_bandwidth metric sets carry a DMA channel in their
      // ":<channel>" suffix (stored in configChannel0). Match by column/row.
      auto configChannel0 = metadata->getConfigChannel0();
      const auto& shimTile = shimConfigMetrics.front().first;
      for (const auto& tc : configChannel0) {
        if ((tc.first.col == shimTile.col) && (tc.first.row == shimTile.row)) {
          bandwidthChannel = tc.second;
          break;
        }
      }
      xrt_core::message::send(severity_level::info, "XRT",
          "AIE dtrace: Using metric set '" + bandwidthMetricSet + "' (channel "
          + std::to_string(bandwidthChannel) + ") from configuration");
    } else {
      xrt_core::message::send(severity_level::info, "XRT",
          "AIE dtrace: No interface tile metrics configured, using default 'peak_read_bandwidth'");
    }

    if (!ctWriter.generateBandwidthCT(outputPath, hwctx, it->second, bandwidthMetricSet, bandwidthChannel))
      return;

    xrt_core::message::send(severity_level::debug, "XRT",
        "AIE dtrace: Bandwidth CT generated for kernel '" + kernel_name
        + "' with metric set '" + bandwidthMetricSet + "'");

    auto* run_impl = static_cast<xrt::run_impl*>(run_impl_ptr);
    try {
      xrt_core::kernel_int::set_dtrace_control_file(run_impl, outputPath);
      std::stringstream msg;
      msg << "AIE dtrace: Set per-run CT file '" << outputPath
          << "' for run uid=" << run_uid << " ctx slot=" << slotIdx;
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
    }
    catch (const std::exception& e) {
      std::stringstream msg;
      msg << "AIE dtrace: Could not set per-run CT file: " << e.what();
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }
  }

}
