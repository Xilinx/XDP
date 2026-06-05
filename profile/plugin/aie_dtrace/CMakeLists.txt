# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2025 Advanced Micro Devices, Inc.  All rights reserved.
#
# Standalone AIE dtrace plugin for VE2/XDNA. Handles bandwidth metric sets only;
# no dependency on aie_profile sources.

if (XDP_VE2_BUILD_CMAKE STREQUAL "yes")
  file(GLOB AIE_DTRACE_PLUGIN_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  )
  set(AIE_DTRACE_UTIL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/util/aie_dtrace_util.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/util/aie_dtrace_util.cpp"
  )
  file(GLOB AIE_DTRACE_IMPL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/ve2/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ve2/*.cpp"
  )

  add_library(xdp_aie_dtrace_plugin_xdna SHARED
    ${AIE_DTRACE_PLUGIN_FILES}
    ${AIE_DTRACE_IMPL_FILES}
    ${AIE_DTRACE_UTIL_FILES}
  )
  add_dependencies(xdp_aie_dtrace_plugin_xdna xdp_core xrt_coreutil)
  target_link_libraries(xdp_aie_dtrace_plugin_xdna PRIVATE xdp_core xrt_coreutil xaiengine aiebu_static)
  target_compile_definitions(xdp_aie_dtrace_plugin_xdna PRIVATE XDP_VE2_BUILD=1 FAL_LINUX="on")
  target_include_directories(xdp_aie_dtrace_plugin_xdna PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${XRT_SOURCE_DIR}/runtime_src/core/common/aiebu/src/cpp/include
    ${XRT_SOURCE_DIR}/runtime_src/core/common/elf
  )
  set_target_properties(xdp_aie_dtrace_plugin_xdna PROPERTIES VERSION ${XRT_VERSION_STRING}
                        SOVERSION ${XRT_SOVERSION})

  install(TARGETS xdp_aie_dtrace_plugin_xdna
    RUNTIME DESTINATION ${XDP_PLUGIN_INSTALL_DIR} COMPONENT ${XRT_COMPONENT}
    LIBRARY DESTINATION ${XDP_PLUGIN_INSTALL_DIR} COMPONENT ${XRT_COMPONENT} ${XRT_NAMELINK_SKIP}
  )
endif()
