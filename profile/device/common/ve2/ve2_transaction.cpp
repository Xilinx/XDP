// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>

#include "ve2_transaction.h"
#include "core/common/message.h"
#include "core/common/aiebu/src/cpp/include/aiebu/aiebu_assembler.h"
#include "core/common/aiebu/src/cpp/include/aiebu/aiebu_error.h"
#include "xrt/experimental/xrt_elf.h"
#include "xrt/experimental/xrt_ext.h"
#include "xrt/experimental/xrt_module.h"
#include "xrt/xrt_hw_context.h"
#include "xrt/xrt_kernel.h"

namespace xdp::aie {
    using severity_level = xrt_core::message::severity_level;

    //----------------------------------------------------------------------
    // VE2Transaction::fullElfKernelName
    // Return the unique kernel name for this transaction's full ELF. Multiple
    // XDP ELFs are add_config'd to the same hw_context, so the name must be
    // unique per ELF to avoid a kernel-name collision. The "XDP_KERNEL" prefix
    // keeps the name recognized by the XDP run-lifecycle hooks.
    //----------------------------------------------------------------------
    std::string VE2Transaction::fullElfKernelName(const std::string& instanceId)
    {
      return std::string("XDP_KERNEL_") + instanceId;
    }

    //----------------------------------------------------------------------
    // VE2Transaction::fullElfKernelHandle
    // Return the name/handle of the full ELF kernel (kernelName:instanceName).
    //----------------------------------------------------------------------
    std::string VE2Transaction::fullElfKernelHandle(const std::string& instanceId)
    {
      return fullElfKernelName(instanceId) + ":" + instanceId;
    }

    //----------------------------------------------------------------------
    // VE2Transaction::loadXdpKernelFullElfConfig
    // Build the xdp_kernel_full_elf config JSON in memory for AIEBU aie2ps_config.
    // Sets instance id -> instanceId, ctrl_code_file -> ./<asmBasename>.
    //----------------------------------------------------------------------
    std::vector<char> VE2Transaction::loadXdpKernelFullElfConfig(const std::string& asmFileName,
                                                                 const std::string& instanceId)
    {
      const std::filesystem::path asmPath{asmFileName};
      const std::string asmRel =
          std::string("./") + asmPath.filename().generic_string();

      const std::string jsonText =
          "{\n"
          "    \"xrt-kernels\": [\n"
          "        {\n"
          "            \"name\" : \"" + fullElfKernelName(instanceId) + "\",\n"
          "            \"arguments\" : [],\n"
          "            \"instance\" : [\n"
          "                {\n"
          "                \"id\" : \"" + instanceId + "\",\n"
          "                \"ctrl_code_file\" : \"" + asmRel + "\"\n"
          "                }\n"
          "            ]\n"
          "        }\n"
          "    ]\n"
          "}\n";

      return {jsonText.begin(), jsonText.end()};
    }

    bool VE2Transaction::initializeTransaction(XAie_DevInst* aieDevInst, std::string tName) 
    {
        setTransactionName(tName);
        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
            "Writing to New Control Code ASM file: " + getAsmFileName());

        try {
            // On VE2 Linux, the default IO backend is Linux IO which tries to open
            // the AIE character device — this fails on XDNA (PCIe NPU) since the
            // AIE is managed by the XDNA driver, not the Linux AIE driver.
            // Explicitly switch to control-code backend before opening the ASM file.
            XAie_SetIOBackend(aieDevInst, XAIE_IO_BACKEND_CONTROLCODE);
            XAie_OpenControlCodeFile(aieDevInst, getAsmFileName().c_str(), 8192);
            XAie_StartNewJob(aieDevInst, XAIE_START_JOB);
            return true;
        }
        catch(const std::exception& e) {
            xrt_core::message::send(xrt_core::message::severity_level::error, "XRT",
                "Error in generating asm File: " + getAsmFileName() + "\n" + e.what());
        }
        xrt_core::message::send(severity_level::warning, "XRT", "AIE Transaction Initialization Failed.");
        return false;
    }

    bool VE2Transaction::completeASM(XAie_DevInst* aieDevInst)
    {
        //
        // 1. End generation of ASM file
        //
        try {
            XAie_EndJob(aieDevInst);
            XAie_EndPage(aieDevInst);
            XAie_CloseControlCodeFile(aieDevInst);
        }
        catch(const std::exception& e) {
            xrt_core::message::send(xrt_core::message::severity_level::error, "XRT",
                "Error in generating ASM file: " + getAsmFileName() + "\n" + e.what());
            return false;
        }
        return true;
    }

    bool VE2Transaction::generateELF()
    {
        //
        // 2. Run AIEBU aie2ps_config to produce a full config ELF from the ASM
        //    control code + an in-memory config JSON. The config names the kernel
        //    (uniquely per transaction) and points its instance at the ASM file.
        //
        try {
            const std::string asmFileName = getAsmFileName();
            if (!std::filesystem::exists(asmFileName))
                throw std::runtime_error("file:" + asmFileName + " not found\n");

            xrt_core::message::send(severity_level::debug, "XRT",
                "AIEBU control ASM: " + asmFileName);

            const std::vector<char> configJson =
                loadXdpKernelFullElfConfig(asmFileName, m_transactionName);

            // The ASM file name may be relative with no directory component
            // (e.g. "AieProfileMetrics1.asm"), in which case parent_path() is
            // empty. std::filesystem::absolute("") throws, so fall back to the
            // current working directory for the absolute lib path.
            const std::filesystem::path asmPath{asmFileName};
            const std::filesystem::path asmParent =
                asmPath.has_parent_path() ? asmPath.parent_path()
                                          : std::filesystem::current_path();
            const std::string asmDir =
                std::filesystem::absolute(asmParent).generic_string();
            const std::vector<std::string> libPaths{std::string("."), asmDir};
            const std::vector<std::string> aiebuFlags{"disabledump"};
            const std::vector<char> emptyCodeBuf;
            const aiebu::aiebu_assembler assembler(aiebu::aiebu_assembler::buffer_type::aie2ps_config,
                                                   emptyCodeBuf,
                                                   aiebuFlags,
                                                   libPaths,
                                                   configJson);

            const auto elfBytes = assembler.get_elf();
            xrt_core::message::send(severity_level::debug, "XRT",
                "AIEBU output ELF bytes: " + std::to_string(elfBytes.size()));

            std::ofstream outElf(getElfFileName(),
                                  std::ios::binary | std::ios::out | std::ios::trunc);
            outElf.write(elfBytes.data(), static_cast<std::streamsize>(elfBytes.size()));
        }
        catch(const aiebu::error& e) {
            xrt_core::message::send(severity_level::error, "XRT",
                "AIEBU error generating Elf file: " + getElfFileName() + "\n" + e.what());
            return false;
        }
        catch(const std::exception& e) {
            xrt_core::message::send(severity_level::error, "XRT",
                "Error in generating Elf file: " + getElfFileName() + "\n" + e.what());
            return false;
        }
        return true;
    }

    bool VE2Transaction::submitELF(xrt::hw_context hwContext) 
    {
        //
        // 3. Submit ELF to microcontroller
        //
        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", 
            "Start New Control Code Elf");
        xrt::elf profileElf;
        try {
            profileElf = xrt::elf(getElfFileName());
        } 
        catch (...) {
            xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
            "Failed to load " + getElfFileName() + ". Cannot configure AIE to profile.");
            return false;
        }

        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", "Elf Object Created");
        xrt::kernel kernel;
        try {
            hwContext.add_config(profileElf);
            kernel = xrt::ext::kernel{hwContext, fullElfKernelHandle(m_transactionName)};
        } catch (const std::exception& e) {
            xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
            std::string("Failed to register ") + getElfFileName() + " with HW Context: " + e.what());
            return false;
        }
        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", "XDP_KERNEL created");
        
        xrt::run run{kernel};
        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", "Kernel run created");
        
        run.start();
        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", "Run started");
        
        run.wait2();
        xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", "Wait done!");
        
        return true;
    }

    bool VE2Transaction::submitTransaction(XAie_DevInst* aieDevInst, xrt::hw_context hwContext) 
    {
        if (!completeASM(aieDevInst))
            return false;
        if (!generateELF())
            return false;
        if (!submitELF(hwContext))
            return false;
        return true;
    }

    int VE2Transaction::getGroupID(int id, xrt::hw_context hwContext)
    {
        xrt::kernel kernel = xrt::ext::kernel(hwContext, fullElfKernelHandle(m_transactionName));
        return kernel.group_id(id);
    }

    // Below functions are required for AIE Trace only
    // AIE Trace requires a flush ELF to force trace packets out of the tiles at end-of-run.  
    //
    // During flush ELF, creation of xrt::kernel calls ip_context::open() which accesses a
    // static map (dev2ips) in xrt_kernel.cpp. This map may already be destroyed during teardown.
    // We split into prepare (creates the kernel during setup when statics are alive) and 
    // run (reuses it at flush time without touching the static map).
    bool VE2Transaction::prepareFlushKernel(xrt::hw_context hwContext)
    {
        xrt_core::message::send(severity_level::info, "XRT",
            "Preparing flush kernel from ELF: " + getElfFileName());
        try {
            xrt::elf flushElf(getElfFileName());
            hwContext.add_config(flushElf);
            m_flushKernel = xrt::ext::kernel{hwContext, fullElfKernelHandle(m_transactionName)};
            m_flushKernelReady = true;
            xrt_core::message::send(severity_level::info, "XRT",
                "Flush kernel prepared successfully.");
            return true;
        } catch (const std::exception& e) {
            xrt_core::message::send(severity_level::warning, "XRT",
                std::string("Failed to prepare flush kernel: ") + e.what());
            m_flushKernelReady = false;
            return false;
        } catch (...) {
            xrt_core::message::send(severity_level::warning, "XRT",
                "Failed to prepare flush kernel (unknown error).");
            m_flushKernelReady = false;
            return false;
        }
    }

    bool VE2Transaction::runFlushKernel()
    {
        if (!m_flushKernelReady) {
            xrt_core::message::send(severity_level::warning, "XRT",
                "Flush kernel was not prepared. Cannot flush AIE trace.");
            return false;
        }
        try {
            xrt_core::message::send(severity_level::debug, "XRT", "Running pre-created flush kernel");
            xrt::run run{m_flushKernel};
            run.start();
            run.wait2();
            xrt_core::message::send(severity_level::debug, "XRT", "Flush run done!");
            return true;
        } catch (const std::exception& e) {
            xrt_core::message::send(severity_level::warning, "XRT",
                std::string("Flush kernel run failed: ") + e.what());
            return false;
        } catch (...) {
            xrt_core::message::send(severity_level::warning, "XRT",
                "Flush kernel run failed (unknown error).");
            return false;
        }
    }
}