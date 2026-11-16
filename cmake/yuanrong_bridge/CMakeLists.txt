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
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(XllmYuanrongBridge LANGUAGES CXX)
endif()

get_filename_component(XLLM_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(YUANRONG_ROOT "" CACHE PATH "DatasystemPrivate static PIC SDK install prefix")
if(YUANRONG_ROOT STREQUAL "")
  message(FATAL_ERROR "Set YUANRONG_ROOT to the installed DS static PIC SDK.")
endif()

# The bridge only exchanges C data with the host. In particular, Torch's ABI=0
# compile options must not override the private SDK's ABI=1 definition.
foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES
                 LINK_LIBRARIES LINK_OPTIONS LINK_DIRECTORIES)
  set_property(DIRECTORY PROPERTY ${property})
endforeach()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(DatasystemPrivate CONFIG REQUIRED
  PATHS
    "${YUANRONG_ROOT}/lib/cmake/DatasystemPrivate"
    "${YUANRONG_ROOT}/lib64/cmake/DatasystemPrivate"
  NO_DEFAULT_PATH)
if(NOT TARGET DatasystemPrivate::client)
  message(FATAL_ERROR "The DS static PIC SDK must provide DatasystemPrivate::client.")
endif()

set(symbols_file "${CMAKE_CURRENT_BINARY_DIR}/xllm_yuanrong_bridge_symbols.map")
file(WRITE "${symbols_file}"
  "{\n  global:\n    xllm_yr_store_create;\n    xllm_yr_store_destroy;\n    xllm_yr_store_mset;\n    xllm_yr_store_mget;\n    xllm_yr_store_last_error;\n  local:\n    *;\n};\n")
add_library(xllm_yuanrong_store_bridge SHARED
  "${XLLM_SOURCE_ROOT}/xllm/core/kv_cache/transfer/yuanrong_store_bridge.cpp")
target_include_directories(xllm_yuanrong_store_bridge PRIVATE "${XLLM_SOURCE_ROOT}/xllm")
target_link_libraries(xllm_yuanrong_store_bridge PRIVATE DatasystemPrivate::client)
set_target_properties(xllm_yuanrong_store_bridge PROPERTIES
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
  # GCC emits optional weak TLS initializers even for trivial thread_locals.
  # AArch64 GNU ld does not support -z nodynamic-undefined-weak. Mark these
  # SDK references hidden so missing initializers resolve to zero locally;
  # any real initializer in the static closure still supplies its definition.
  get_property(private_targets DIRECTORY PROPERTY IMPORTED_TARGETS)
  set(private_archives "")
  foreach(private_target IN LISTS private_targets)
    if(private_target MATCHES "^DatasystemPrivate::")
      get_target_property(private_type "${private_target}" TYPE)
      if(private_type STREQUAL "STATIC_LIBRARY")
        get_target_property(archive "${private_target}" IMPORTED_LOCATION)
        list(APPEND private_archives "${archive}")
      endif()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES private_archives)
  execute_process(COMMAND "${YUANRONG_NM}" --undefined-only --format=posix ${private_archives}
    RESULT_VARIABLE nm_status OUTPUT_VARIABLE sdk_undefined ERROR_VARIABLE nm_error)
  if(NOT nm_status EQUAL 0)
    message(FATAL_ERROR "Cannot inspect DS SDK TLS references: ${nm_error}")
  endif()
  string(REPLACE "\n" ";" sdk_undefined_lines "${sdk_undefined}")
  set(tls_initializers "")
  foreach(line IN LISTS sdk_undefined_lines)
    if(line MATCHES "^(_ZTH[A-Za-z0-9_]+)[ \t]+w([ \t]|$)")
      list(APPEND tls_initializers "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES tls_initializers)
  set(tls_source "${CMAKE_CURRENT_BINARY_DIR}/private_sdk_tls.cpp")
  file(WRITE "${tls_source}" "// Generated from the private SDK's optional TLS initializers.\n")
  foreach(symbol IN LISTS tls_initializers)
    file(APPEND "${tls_source}" "__asm__(\".weak ${symbol}\\n.hidden ${symbol}\");\n")
  endforeach()
  target_sources(xllm_yuanrong_store_bridge PRIVATE "${tls_source}")
  add_custom_command(TARGET xllm_yuanrong_store_bridge POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DBRIDGE_FILE=$<TARGET_FILE:xllm_yuanrong_store_bridge>"
      "-DREADELF=${YUANRONG_READELF}"
      "-DNM=${YUANRONG_NM}"
      -P "${XLLM_SOURCE_ROOT}/cmake/check_yuanrong_bridge.cmake"
    COMMENT "Auditing YuanRong bridge dependencies and exported symbols"
    VERBATIM)
endif()

if(CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(bridge_output_dir "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
elseif(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(bridge_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
else()
  set(bridge_output_dir "${CMAKE_BINARY_DIR}/xllm")
endif()
set_target_properties(xllm_yuanrong_store_bridge PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${bridge_output_dir}")

if(GENERATE_SO)
  set(bridge_install_dir "${CMAKE_INSTALL_LIBDIR}")
else()
  set(bridge_install_dir "${CMAKE_INSTALL_BINDIR}")
endif()
if(NOT bridge_install_dir)
  set(bridge_install_dir lib)
endif()
install(TARGETS xllm_yuanrong_store_bridge LIBRARY DESTINATION "${bridge_install_dir}")
