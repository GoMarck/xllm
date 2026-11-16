# Copyright 2026 The xLLM Authors.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(XLLM_ATB_CUSTOMIZE_ROOT "$ENV{XLLM_ATB_CUSTOMIZE_ROOT}" CACHE PATH
    "ATB customize installation prefix matching the PyTorch C++ ABI")
if(USE_CXX11_ABI)
  set(_atb_customize_abi 1)
else()
  set(_atb_customize_abi 0)
endif()
set(_atb_customize_source_install
    "${PROJECT_SOURCE_DIR}/third_party/xllm_ops/atb_customize/output/ops_customize/cxx_abi_${_atb_customize_abi}")

find_path(ATBCustomize_INCLUDE_DIR customize/customize_op_params.h
  HINTS "${XLLM_ATB_CUSTOMIZE_ROOT}/include"
        "${_atb_customize_source_install}/include"
        "$ENV{ATB_HOME_PATH}/include")
find_library(ATBCustomize_LIBRARY NAMES atb_customize
  HINTS "${XLLM_ATB_CUSTOMIZE_ROOT}/lib"
        "${XLLM_ATB_CUSTOMIZE_ROOT}/lib64"
        "${_atb_customize_source_install}/lib"
        "$ENV{ATB_HOME_PATH}/lib")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ATBCustomize
  REQUIRED_VARS ATBCustomize_INCLUDE_DIR ATBCustomize_LIBRARY
  REASON_FAILURE_MESSAGE
    "Build third_party/xllm_ops/atb_customize/build.sh with --use_cxx11_abi=${_atb_customize_abi}, or set XLLM_ATB_CUSTOMIZE_ROOT to a matching installation.")

if(ATBCustomize_FOUND AND NOT TARGET atb_customize)
  add_library(atb_customize SHARED IMPORTED GLOBAL)
  set_target_properties(atb_customize PROPERTIES
    IMPORTED_LOCATION "${ATBCustomize_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ATBCustomize_INCLUDE_DIR}")
endif()
mark_as_advanced(ATBCustomize_INCLUDE_DIR ATBCustomize_LIBRARY)
