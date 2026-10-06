# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2024-2025 Advanced Micro Devices, Inc.  All rights reserved.
#

# =======================================================================
# This builds the AIE Halt Plugin to add Debug Halts for all AIE Cores at 
# the start. It is currently built on Client and VE2
# =========================================================================

if (XDP_CLIENT_BUILD_CMAKE STREQUAL "yes")
  set(IMPL_DIR "${PROFILE_DIR}/plugin/aie_halt/clientDev")

elseif (XDP_VE2_BUILD_CMAKE STREQUAL "yes")
  set(IMPL_DIR "${PROFILE_DIR}/plugin/aie_halt/ve2")
  
endif()

file(GLOB XDP_AIE_HALT_PLUGIN_FILES
  "${PROFILE_DIR}/plugin/aie_halt/*.h"
  "${PROFILE_DIR}/plugin/aie_halt/*.cpp"
  "${IMPL_DIR}/*.h"
  "${IMPL_DIR}/*.cpp"
)

file(GLOB XDP_DEVICE_COMMON_FILES
  "${PROFILE_DIR}/device/common/*.h"
  "${PROFILE_DIR}/device/common/*.cpp"
)

if (XDP_CLIENT_BUILD_CMAKE STREQUAL "yes")
  xrt_configure_version_file(xdp_aie_halt_plugin SHARED)
  add_library(xdp_aie_halt_plugin SHARED xdp_aie_halt_plugin-version.rc ${XDP_AIE_HALT_PLUGIN_FILES} ${XDP_DEVICE_COMMON_FILES})
  add_dependencies(xdp_aie_halt_plugin xdp_core xrt_coreutil)
  target_link_libraries(xdp_aie_halt_plugin PRIVATE xdp_core xrt_coreutil aie_codegen)
  target_compile_definitions(xdp_aie_halt_plugin PRIVATE XDP_CLIENT_BUILD=1 XDP_USE_AIE_CODEGEN=1 -DXAIE_FEATURE_MSVC)
  target_include_directories(xdp_aie_halt_plugin PRIVATE ${AIE_CODEGEN_DIR}/include)
  set_target_properties(xdp_aie_halt_plugin PROPERTIES VERSION ${XRT_VERSION_STRING} SOVERSION ${XRT_SOVERSION})

  install (TARGETS xdp_aie_halt_plugin
    RUNTIME DESTINATION ${XDP_PLUGIN_INSTALL_DIR} COMPONENT ${XRT_COMPONENT}
    LIBRARY DESTINATION ${XDP_PLUGIN_INSTALL_DIR} COMPONENT ${XRT_COMPONENT} ${XRT_NAMELINK_SKIP}
  )

elseif (XDP_VE2_BUILD_CMAKE STREQUAL "yes")
  # For ve2 the halt control code is generated on the fly using aie-codegen
  # (with a prebuilt elf loaded as a fallback), so the aie-codegen library and
  # the shared device/common transaction helpers are required. Note the
  # XDP_DEVICE_COMMON_FILES glob does not recurse into device/common/ve2, so
  # ve2_transaction.cpp must be added explicitly.
  add_library(xdp_aie_halt_plugin SHARED ${XDP_AIE_HALT_PLUGIN_FILES} ${XDP_DEVICE_COMMON_FILES}
    "${PROFILE_DIR}/device/common/ve2/ve2_transaction.cpp")
  add_dependencies(xdp_aie_halt_plugin xdp_core xrt_coreutil)

  target_link_libraries(xdp_aie_halt_plugin PRIVATE xdp_core xrt_coreutil aie_codegen aiebu_static)
  # -Bsymbolic: ensures XAie_* calls within this plugin resolve to the
  # statically-linked aie_codegen rather than being interposed by the
  # system libxaiengine.so.3 (required by libxrt_core.so.2)
  target_link_options(xdp_aie_halt_plugin PRIVATE -Wl,-Bsymbolic)
  target_compile_definitions(xdp_aie_halt_plugin PRIVATE XDP_VE2_BUILD=1 XDP_USE_AIE_CODEGEN=1 FAL_LINUX="on")
  target_include_directories(xdp_aie_halt_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${AIEFAL_DIR}
    ${AIEBU_SOURCE_DIR}/src/cpp/include
    ${AIE_CODEGEN_DIR}/include
    ${XRT_SOURCE_DIR}/runtime_src/core/common/elf
  )

  set_target_properties(xdp_aie_halt_plugin PROPERTIES VERSION ${XRT_VERSION_STRING} SOVERSION ${XRT_SOVERSION})

  install (TARGETS xdp_aie_halt_plugin
    RUNTIME DESTINATION ${XDP_PLUGIN_INSTALL_DIR} COMPONENT ${XRT_COMPONENT}
    LIBRARY DESTINATION ${XDP_PLUGIN_INSTALL_DIR} COMPONENT ${XRT_COMPONENT} ${XRT_NAMELINK_SKIP}
  )

# Else, on edge-aarch64 don't build at all

endif()
