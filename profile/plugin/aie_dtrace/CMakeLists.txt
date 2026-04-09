# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2025 Advanced Micro Devices, Inc.  All rights reserved.
#
# Link this plugin with the same AIE profile *object sources* as
# xdp_aie_profile_plugin_xdna (see plugin/aie_profile/CMakeLists.txt, XDP_VE2_BUILD),
# except:
#   - no aie_profile_plugin*, aie_profile_cb*, writer/aie_profile/*
#   - no ve2/aie_profile* (replaced by aie_dtrace/ve2/*)
#
# If load fails with "undefined symbol" from AieProfileMetadata / parser / elf,
# add the missing .cpp here or in AIE_DTRACE_PROFILE_CORE_SOURCES below.
# ELF serialization for aiebu CT: ve2/elf_helper.cpp (same role as aie_profile/ve2/elf_helper.cpp).
#

if (XDP_VE2_BUILD_CMAKE STREQUAL "yes")
  set(XDP_PROFILE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

  file(GLOB AIE_DTRACE_PLUGIN_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  )
  file(GLOB AIE_DTRACE_UTIL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/util/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/util/*.cpp"
  )
  file(GLOB AIE_DTRACE_IMPL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/ve2/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ve2/*.cpp"
  )

  # Explicit .cpp from aie_profile tree required by metadata + CT/ELF (not covered by util/config globs).
  set(AIE_DTRACE_PROFILE_CORE_SOURCES
    "${XDP_PROFILE_ROOT}/plugin/aie_profile/aie_profile_metadata.cpp"
    "${XDP_PROFILE_ROOT}/plugin/aie_profile/aie_profile_metadata_json.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ve2/elf_helper.cpp"
  )

  file(GLOB AIE_PROFILE_UTIL_FILES
    "${XDP_PROFILE_ROOT}/plugin/aie_base/*"
    "${XDP_PROFILE_ROOT}/plugin/aie_base/generations/*"
    "${XDP_PROFILE_ROOT}/plugin/aie_profile/util/aie_profile_util.h"
    "${XDP_PROFILE_ROOT}/plugin/aie_profile/util/aie_profile_util.cpp"
  )
  file(GLOB AIE_PROFILE_CONFIG_FILES
    "${XDP_PROFILE_ROOT}/plugin/aie_profile/util/aie_profile_config.h"
    "${XDP_PROFILE_ROOT}/plugin/aie_profile/util/aie_profile_config.cpp"
  )
  file(GLOB AIE_JSON_PARSER_FILES
    "${XDP_PROFILE_ROOT}/database/parser/*.h"
    "${XDP_PROFILE_ROOT}/database/parser/*.cpp"
  )

  add_library(xdp_aie_dtrace_plugin_xdna SHARED
    ${AIE_DTRACE_PLUGIN_FILES}
    ${AIE_DTRACE_IMPL_FILES}
    ${AIE_DTRACE_UTIL_FILES}
    ${AIE_DTRACE_PROFILE_CORE_SOURCES}
    ${AIE_PROFILE_UTIL_FILES}
    ${AIE_PROFILE_CONFIG_FILES}
    ${AIE_JSON_PARSER_FILES}
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
