// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ve2.h"
#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ct_writer.h"
#include "xdp/profile/plugin/aie_dtrace/ve2/elf_helper.h"
#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"

#include "core/common/api/hw_context_int.h"
#include "core/common/api/kernel_int.h"
#include "core/common/config_reader.h"
#include "core/common/message.h"
#include "core/common/shim/hwctx_handle.h"

#include <cctype>

#include "xdp/profile/database/static_info/aie_util.h"

#include <boost/property_tree/ptree.hpp>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <system_error>

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  namespace {

    // Kernel names reach the filesystem as part of the CT file name and may
    // contain separators or other characters the host filesystem rejects.
    std::string sanitizeForFilename(const std::string& name)
    {
      if (name.empty())
        return "kernel";

      std::string out;
      out.reserve(name.size());
      for (const char c : name)
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                      ? c : '_');
      return out;
    }

    // Hands a CT file to XRT for this run object. "what" names the caller's
    // unit of work for the log line, since this runs both at run construction
    // and at run start.
    void programCT(void* run_impl_ptr, uint32_t run_uid, const std::string& ctFile,
                   const std::string& what)
    {
      try {
        xrt_core::kernel_int::set_dtrace_control_file(
            static_cast<xrt::run_impl*>(run_impl_ptr), ctFile);

        std::stringstream msg;
        msg << "AIE dtrace: " << what << " (run uid=" << run_uid << ") will use CT '"
            << ctFile << "'";
        xrt_core::message::send(severity_level::info, "XRT", msg.str());
      }
      catch (const std::exception& e) {
        std::stringstream msg;
        msg << "AIE dtrace: Could not set CT file '" << ctFile << "' for run uid=" << run_uid
            << ": " << e.what();
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      }
    }

  } // namespace

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

  void AieDtrace_VE2Impl::generateCTsForRun(void* run_impl_ptr, void* hwctx, uint32_t run_uid,
                                            const std::string& kernel_name,
                                            void* elf_handle)
  {
    if (!xrt_core::config::get_aie_dtrace())
      return;

    const auto& selections = metadata->getMetricSelections();
    if (selections.empty())
      return;

    if (std::all_of(selections.begin(), selections.end(),
                    [](const MetricSelection& s) { return s.empty(); })) {
      xrt_core::message::send(severity_level::info, "XRT",
          "AIE dtrace: No metrics configured; skipping CT generation.");
      return;
    }

    std::string ctFile;

    {
      std::lock_guard<std::mutex> lock(m_mutex);

      // Every run object of a kernel shares the same ELF, so the CT files only
      // have to be built for the first one.
      if (!m_ct_files.count(kernel_name))
        generateCTFiles(hwctx, run_uid, kernel_name, elf_handle);

      // A single metric set describes every inference, so the CT is programmed
      // once here and run start has nothing left to select. A multi-inference
      // sequence cannot be resolved yet, since which CT applies depends on the
      // inference number, which is only known at start.
      if (!metadata->isMultiInference()) {
        if (const auto* ctFiles = findCTFiles(kernel_name))
          ctFile = ctFiles->front();
      }
    }

    if (!ctFile.empty())
      programCT(run_impl_ptr, run_uid, ctFile, "Kernel '" + kernel_name + "'");
  }

  // Builds one CT per configured inference and records them under kernel_name.
  void AieDtrace_VE2Impl::generateCTFiles(void* hwctx, uint32_t run_uid,
                                          const std::string& kernel_name, void* elf_handle)
  {
    const auto& selections = metadata->getMetricSelections();

    computeOpLocations(elf_handle, kernel_name);

    auto opLocations = m_op_locations_cache.find(kernel_name);
    if (opLocations == m_op_locations_cache.end() || opLocations->second.empty()) {
      xrt_core::message::send(severity_level::debug, "XRT",
          "AIE dtrace: No op_locations for kernel '" + kernel_name + "'; skipping CT generation.");
      return;
    }

    auto ctx = xrt_core::hw_context_int::create_hw_context_from_implementation(hwctx);
    const auto slotIdx = static_cast<xrt_core::hwctx_handle*>(ctx)->get_slotidx();

    const auto partition = aie::dtrace::getPartitionGeometry(hwctx);
    AieDtraceCTWriter ctWriter(db, metadata, deviceID, partition.startCol);

    std::vector<std::string> ctFiles(selections.size());
    size_t generated = 0;

    for (size_t i = 0; i < selections.size(); ++i) {
      const auto& selection = selections[i];

      if (selection.empty()) {
        xrt_core::message::send(severity_level::info, "XRT",
            "AIE dtrace: No metrics configured for inference "
            + std::to_string(metadata->getStartInference() + i)
            + " of kernel '" + kernel_name + "'; no CT will be generated for it.");
        continue;
      }

      const std::string filename = "aie_dtrace_ctx_" + std::to_string(slotIdx)
                                 + "_" + sanitizeForFilename(kernel_name)
                                 + "_inference_"
                                 + std::to_string(metadata->getStartInference() + i)
                                 + ".ct";
      const auto finalPath = std::filesystem::current_path() / filename;

      // Written under a temporary name and renamed into place so a run start on
      // another thread can never hand aiebu a half-written CT file.
      const auto tempPath = std::filesystem::path(finalPath).concat(
          ".tmp." + std::to_string(run_uid));

      if (!ctWriter.generateCT(tempPath.string(), hwctx, opLocations->second, selection))
        continue;

      std::error_code ec;
      std::filesystem::rename(tempPath, finalPath, ec);
      if (ec) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "AIE dtrace: Could not move generated CT into place ('" + finalPath.string()
            + "'): " + ec.message());
        std::filesystem::remove(tempPath, ec);
        continue;
      }

      ctFiles[i] = finalPath.string();
      ++generated;

      xrt_core::message::send(severity_level::debug, "XRT",
          "AIE dtrace: CT generated for kernel '" + kernel_name + "' inference "
          + std::to_string(metadata->getStartInference() + i)
          + " (" + selection.describe() + "): " + ctFiles[i]);
    }

    if (generated == 0) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE dtrace: No CT files could be generated for kernel '" + kernel_name
          + "'; dtrace data will not be collected for it.");
      return;
    }

    aie::dtrace::initDtraceOutputConfig();
    m_ct_files.emplace(kernel_name, std::move(ctFiles));
  }

  const std::vector<std::string>*
  AieDtrace_VE2Impl::findCTFiles(const std::string& kernel_name) const
  {
    auto itr = m_ct_files.find(kernel_name);
    return (itr == m_ct_files.end()) ? nullptr : &itr->second;
  }

  void AieDtrace_VE2Impl::applyCTForRun(void* run_impl_ptr, void* /*hwctx*/, uint32_t run_uid,
                                        const std::string& kernel_name)
  {
    if (!xrt_core::config::get_aie_dtrace())
      return;

    // A single metric set was programmed onto this run at construction and
    // covers every one of its inferences, so there is nothing to select here.
    if (!metadata->isMultiInference())
      return;

    std::string ctFile;
    uint64_t inferenceNumber = 0;

    {
      std::lock_guard<std::mutex> lock(m_mutex);

      // 1-based, matching how start_inference counts.
      inferenceNumber = ++m_inference_counts[kernel_name];

      const auto* ctFiles = findCTFiles(kernel_name);
      if (!ctFiles) {
        // Only on the kernel's first inference: an application that runs
        // thousands of them should not get thousands of identical warnings.
        if (inferenceNumber == 1)
          xrt_core::message::send(severity_level::warning, "XRT",
              "AIE dtrace: No CT files were generated for kernel '" + kernel_name
              + "'; its inferences will not be profiled.");
        return;
      }

      const uint64_t startInference = metadata->getStartInference();

      if (inferenceNumber < startInference) {
        xrt_core::message::send(severity_level::debug, "XRT",
            "AIE dtrace: Inference " + std::to_string(inferenceNumber) + " of kernel '"
            + kernel_name + "' is before start_inference ("
            + std::to_string(startInference) + "); not profiled.");
      }
      else {
        const uint64_t index = inferenceNumber - startInference;

        if (index >= ctFiles->size()) {
          // Warn only on the first inference past the window; a long-running
          // application would otherwise log this on every remaining one.
          if (index == ctFiles->size())
            xrt_core::message::send(severity_level::warning, "XRT",
                "AIE dtrace: Kernel '" + kernel_name + "' has run more inferences than the "
                + std::to_string(ctFiles->size()) + " configured in profile_runs; "
                "inference " + std::to_string(inferenceNumber)
                + " onwards will not be profiled.");
        }
        else {
          ctFile = (*ctFiles)[index];
        }
      }
    }

    // An empty selection means this inference is outside the configured window,
    // or its CT could not be generated. The run object may well be the same one
    // a profiled inference used, so dtrace has to be turned off explicitly
    // rather than just left alone.
    if (ctFile.empty()) {
      try {
        xrt_core::kernel_int::clear_dtrace_control_file(
            static_cast<xrt::run_impl*>(run_impl_ptr));
      }
      catch (const std::exception& e) {
        std::stringstream msg;
        msg << "AIE dtrace: Could not disable dtrace for run uid=" << run_uid
            << ": " << e.what();
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      }
      return;
    }

    programCT(run_impl_ptr, run_uid, ctFile,
              "Inference " + std::to_string(inferenceNumber) + " of kernel '" + kernel_name + "'");
  }

  void AieDtrace_VE2Impl::reportUnusedSelections()
  {
    if (!metadata->isMultiInference())
      return;

    const uint64_t startInference = metadata->getStartInference();
    const uint64_t configured = metadata->getMetricSelections().size();

    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto& entry : m_ct_files) {
      const auto& kernel_name = entry.first;
      const uint64_t ran = m_inference_counts.count(kernel_name)
          ? m_inference_counts.at(kernel_name) : 0;
      const uint64_t profiled = (ran < startInference) ? 0
          : std::min(ran - startInference + 1, configured);

      if (profiled >= configured)
        continue;

      std::stringstream msg;
      msg << "AIE dtrace: Kernel '" << kernel_name << "' ran " << ran
          << " inferences, so only " << profiled << " of the " << configured
          << " configured profile_runs were collected. Run the kernel at least "
          << (startInference + configured - 1)
          << " times to collect the whole sequence. Missing:";
      for (uint64_t i = profiled; i < configured; ++i)
        msg << "\n  inference " << (startInference + i) << ": "
            << metadata->getMetricSelections()[i].describe();
      xrt_core::message::send(severity_level::warning, "XRT", msg.str());
    }
  }

}
