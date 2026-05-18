// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef VE2_TRANSACTION_DOT_H
#define VE2_TRANSACTION_DOT_H

#include <cstdint>
#include <string>
#include <vector>

#include "xrt/xrt_hw_context.h"
#include "xrt/xrt_kernel.h"

#if defined(XDP_USE_AIE_CODEGEN)
  extern "C" {
    #include <xaiegbl_dynlink.h>
    #include <aie_codegen.h>
    #include <aie_codegen_inc/xaiegbl_params.h>
  }
#endif

/**
 * @brief VE2Transaction class for generating and submitting VE2 XDNA transactions
 * 
 * This class is used to generate and submit VE2 transactions. It is used to generate the ASM file, the ELF file, and submit the transaction.
 * 
 */

namespace xdp::aie {
  class VE2Transaction {
    public: 
      VE2Transaction() {};
      bool initializeTransaction(XAie_DevInst* aieDevInst, std::string tName);
      bool submitTransaction(XAie_DevInst* aieDevInst, xrt::hw_context hwContext);
      bool completeASM(XAie_DevInst* aieDevInst);
      bool generateELF();
      bool submitELF(xrt::hw_context hwContext);

      void setTransactionName(std::string newTransactionName) {m_transactionName = newTransactionName;}
      std::string getAsmFileName() { return m_transactionName + ".asm"; }
      std::string getElfFileName() { return m_transactionName + ".elf"; }
      int getGroupID(int id, xrt::hw_context hwContext) {
        xrt::kernel kernel = xrt::kernel(hwContext, "XDP_KERNEL"); 
        return kernel.group_id(id); 
      }

      // Below functions are required for AIE Trace only
      // AIE Trace requires a flush ELF to force trace packets out of the tiles at end-of-run.  
      //
      // During flush ELF, creation of xrt::kernel calls ip_context::open() which accesses a
      // static map (dev2ips) in xrt_kernel.cpp. This map may already be destroyed during teardown.
      // We split into prepare (creates the kernel during setup when statics are alive) and 
      // run (reuses it at flush time without touching the static map).
      bool prepareFlushKernel(xrt::hw_context hwContext);
      bool runFlushKernel();

    private:
      std::string m_transactionName;
      std::vector<uint8_t> m_columns;
      std::vector<uint8_t> m_rows;
      std::vector<uint64_t> m_offsets;
      std::vector<uint32_t> m_values;

      // AIE trace flush kernel, pre-created during setup
      xrt::kernel m_flushKernel;
      bool m_flushKernelReady = false;
  };

} // namespace xdp::aie

#endif