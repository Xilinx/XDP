<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved. -->

# XRT Debug/Profile

-------------------------------------------------------------------------------

The XRT Debug/Profile libraries are designed to be built and used in
conjunction with XRT and not as a standalone tool.  This set of loadable
modules implement debug/profiling functionality on host code, PL constructs,
and AIE components.

User controls are through the xrt.ini file.  Support exists for both PCIe
based platforms and embedded platforms.

-------------------------------------------------------------------------------

XRT, and subsequently XDP, is run on a variety of platforms with varying
hardware and capabilities.  Specifically, XDP supports the following:

1. Edge Hardware (boards like the vck190 and vek280).
* Host code on edge using load_xclbin.
* Host code on edge using hw_context.
* Supported features include host trace, PL profiling and trace, AIE profiling and trace, AIE status, PL deadlock detection.

2. Edge Hardware Emulation (QEMU + XSim for vck190 and vek280)
* Host code on edge emulation using load_xclbin.
* Host code on edge emulation using hw_context.
* Supported features include host trace, PL profiling and trace, AIE profiling and trace

3. Alveo Hardware (boards like the u250 and u55c)
* Host code on Alveo using load_xclbin.
* Supported features include host trace, PL profiling and trace

4. Alveo Hardware Emulation
* Host code on Alveo using load_xclbin.
* Supported features include host trace, PL profiling and trace

5. Client hardware - Windows
* Host code written for VAIML designs using hw_contexts.  Communication with
    the hardware must pass along hardware context IDs and use tranasactions
    (or elf) to configure the AIE
* Supported features include host trace, user events, AIE profiling and trace,
    and ML timeline.

6. Client hardware - Linux
* Host code written for VAIML designs using hw_contexts.  Communication with
    the hardware must pass along hardware context IDs and use tranasactions
    (or elf) to configure the AIE
* Supported features include host trace, user events, AIE profiling and trace,
    and ML timeline.

7. VE2 (Vitis)
* Host code written for Vitis designs using load_xclbin
* Host code written for Vitis designs using hw_context.
* Supported features include host trace, user events, AIE profiling and trace.
    All control of the AIE hardware is done through direct calls to aie driver
    and zocl.

8. VE2 (VAIML)
* Host code written for VAIML designs using hw_context.
* Supported features include host trace, user events, AIE profiling and trace.
    All control of the AIE hardware is done through direct calls to aie driver
    and zocl.

Plugins:

* aie_base : No feature
* aie_debug : No feature
* aie_halt : Internal, used with ML Debugger on Client
* aie_pc : No feature
* aie_profile : Used on Client, Edge, Alveo, hardware emulation
* aie_status : Used on Edge, Alveo, hardware emulation
* aie_trace : Used on Client, Edge, Alveo, hardware emulation
* device_offload : PL trace and counters, used on Edge, Alveo, and hardware emulation
* hal : host trace used on Edge and Alveo, hardware emualation
* hal_api_interface : Called from user host code to read PL device counters.  Available on Edge and Alveo
* lop : Low-overhead OpenCL trace of host code.  Available on Edge, Alveo, hardware emulation
* ml_timeline : Available on Client and VE2 (VAIML)
* native : host trace used on Edge, Alveo, and Client, hardware emulation
* noc : No feature
* opencl : OpenCL trace of host code.  Available on Edge, Alveo, hardware emulation
* pl_deadlock: Detect and interpret PL deadlock.  Available on Edge, Alveo
* power: Available on Alveo
* system_compiler : No feature
* user : Called from user host code.  Available on Edge, Alveo, Client, hardware emulation.
* vart : No feature
* vp_base : No feature
