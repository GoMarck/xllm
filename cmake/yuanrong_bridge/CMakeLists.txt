# Copyright 2026 The xLLM Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://github.com/xLLM-AI/xllm/blob/main/LICENSE
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.26)
project(XllmYuanrongBridge LANGUAGES CXX)

get_filename_component(XLLM_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED DS_STATIC_SDK_JOBS AND NOT "$ENV{MAX_JOBS}" STREQUAL "")
  set(DS_STATIC_SDK_JOBS "$ENV{MAX_JOBS}")
endif()
add_subdirectory("${XLLM_SOURCE_ROOT}/third_party/yuanrong-datasystem/cmake/static-sdk" ds-sdk)

set(symbols_file "${CMAKE_CURRENT_BINARY_DIR}/xllm_yuanrong_bridge_symbols.map")
file(CONFIGURE OUTPUT "${symbols_file}" CONTENT
  "{\n  global:\n    xllm_yr_store_create;\n    xllm_yr_store_destroy;\n    xllm_yr_store_mset;\n    xllm_yr_store_mget;\n    xllm_yr_store_thread_error;\n  local:\n    *;\n};\n")
add_library(xllm_yuanrong_store_bridge SHARED
  "${XLLM_SOURCE_ROOT}/xllm/core/kv_cache/transfer/yuanrong_store_bridge.cpp")
target_include_directories(xllm_yuanrong_store_bridge PRIVATE "${XLLM_SOURCE_ROOT}/xllm")
target_link_libraries(xllm_yuanrong_store_bridge PRIVATE Datasystem::client_static)
set_target_properties(xllm_yuanrong_store_bridge PROPERTIES
  CXX_STANDARD 17
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN"
  INSTALL_RPATH_USE_LINK_PATH OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(xllm_yuanrong_store_bridge PRIVATE
    "LINKER:--as-needed"
    "LINKER:--exclude-libs,ALL"
    "LINKER:-z,defs"
    "LINKER:--version-script=${symbols_file}")
  set_property(TARGET xllm_yuanrong_store_bridge APPEND PROPERTY LINK_DEPENDS
    "${symbols_file}" "${XLLM_SOURCE_ROOT}/cmake/check_yuanrong_bridge.cmake")
  find_program(YUANRONG_READELF NAMES "${CMAKE_READELF}" readelf llvm-readelf REQUIRED)
  find_program(YUANRONG_NM NAMES "${CMAKE_NM}" nm llvm-nm REQUIRED)
  add_custom_command(TARGET xllm_yuanrong_store_bridge POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DBRIDGE_FILE=$<TARGET_FILE:xllm_yuanrong_store_bridge>"
      "-DREADELF=${YUANRONG_READELF}"
      "-DNM=${YUANRONG_NM}"
      -P "${XLLM_SOURCE_ROOT}/cmake/check_yuanrong_bridge.cmake"
    COMMENT "Auditing YuanRong bridge dependencies and exported symbols"
    VERBATIM)
endif()

if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set_target_properties(xllm_yuanrong_store_bridge PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  else()
    set_target_properties(xllm_yuanrong_store_bridge PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/xllm")
  endif()
endif()

include(GNUInstallDirs)
if(PROJECT_IS_TOP_LEVEL OR GENERATE_SO)
  install(TARGETS xllm_yuanrong_store_bridge LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
else()
  install(TARGETS xllm_yuanrong_store_bridge LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
