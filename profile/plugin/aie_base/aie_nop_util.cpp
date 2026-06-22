// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved

#include "xdp/profile/plugin/aie_base/aie_nop_util.h"

#include "core/include/xrt/experimental/xrt_elf.h"
#include "core/include/xrt/experimental/xrt_ext.h"
#include "core/include/xrt/experimental/xrt_module.h"
#include "core/common/api/hw_context_int.h"
#include "core/common/message.h"
#include "xrt/xrt_kernel.h"

namespace xdp::aie {

  using severity_level = xrt_core::message::severity_level;

  bool submitNopElf(void* handle)
  {
    xrt_core::message::send(severity_level::debug, "XRT",
              "In submitNopElf, going to load nop code Elf");

    std::string inputCtrlCode = "nop.elf";

    xrt::hw_context hwContext = xrt_core::hw_context_int::create_hw_context_from_implementation(handle);

    xrt::elf nopElf;
    try {
      nopElf = xrt::elf(inputCtrlCode);
    } catch (...) {
      std::string msg = "Failed to load " + inputCtrlCode + " for AIE configuration.";
      xrt_core::message::send(severity_level::warning, "XRT", msg);
      return false;
    }

    // Each flow attaches the nop ELF differently. The Full-ELF flow registers
    // it via add_config() so the name-only ext::kernel constructor resolves it.
    // The xclbin flow uses an explicit xrt::module instead: routing nop.elf
    // through add_config() would make XRT try to parse xrt configuration notes
    // it does not carry ("ELF is missing xrt configuration info").
    xrt::kernel krnl;
    try {
      if (xrt_core::hw_context_int::get_elf_flow(hwContext)) {
        // Register the nop ELF on the hw_context so the ext::kernel
        // constructor can resolve the kernel name from the configured
        // ELF set.
        hwContext.add_config(nopElf);
        krnl = xrt::ext::kernel{hwContext, "XDP_KERNEL:IPUV1CNN"};
      }
      else {
        xrt::module mod{nopElf};
        krnl = xrt::ext::kernel{hwContext, mod, "XDP_KERNEL:{IPUV1CNN}"};
      }
    } catch (const std::exception& e) {
      std::string msg = "XDP_KERNEL not found in HW Context. Cannot configure nop code. Error: ";
      msg += e.what();
      xrt_core::message::send(severity_level::warning, "XRT", msg);
      return false;
    } catch (...) {
      xrt_core::message::send(severity_level::warning, "XRT",
                "XDP_KERNEL not found in HW Context. Cannot configure nop code.");
      return false;
    }

    xrt_core::message::send(severity_level::debug, "XRT",
              "New Kernel Object for XDP_KERNEL created for running nop code Elf");

    xrt::run rn{krnl};
    rn.start();
    xrt_core::message::send(severity_level::debug, "XRT",
              "nop code run start, going to wait");

    rn.wait2();
    xrt_core::message::send(severity_level::debug, "XRT",
              "nop code run wait completed, proceeding to configuration");

    return true;
  }

}  // namespace xdp::aie

