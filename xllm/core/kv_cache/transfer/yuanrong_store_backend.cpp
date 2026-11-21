/* Copyright 2026 The xLLM Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://github.com/xLLM-AI/xllm/blob/main/LICENSE

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "core/kv_cache/transfer/yuanrong_store_backend.h"

#include <dlfcn.h>
#include <glog/logging.h>

#include <string>
#include <utility>

#include "core/kv_cache/transfer/store_config_validation.h"

namespace xllm {
namespace {

constexpr char kDefaultBridgeLibrary[] = "libxllm_yuanrong_store_bridge.so";

// KV keys include binary hashes. Hex encoding preserves them in the DS key
// charset.
std::string encode_key(const std::string& key) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(key.size() * 2);
  for (unsigned char byte : key) {
    encoded.push_back(kHex[byte >> 4]);
    encoded.push_back(kHex[byte & 0x0F]);
  }
  return encoded;
}

}  // namespace

YuanRongStoreBackend::YuanRongStoreBackend(YuanRongStoreConfig config)
    : config_(std::move(config)) {}

YuanRongStoreBackend::~YuanRongStoreBackend() {
  if (bridge_client_ != nullptr) {
    bridge_destroy_(bridge_client_);
    bridge_client_ = nullptr;
  }
  if (bridge_handle_ != nullptr) {
    dlclose(bridge_handle_);
    bridge_handle_ = nullptr;
  }
}

bool YuanRongStoreBackend::marshal_buffers(
    const std::vector<StoreBuffer>& buffers,
    std::vector<std::vector<uint64_t>>* size_storage,
    std::vector<BridgeBuffer>* bridge_buffers) {
  // reserve() before emplace_back() keeps the inner vectors' data pointers
  // stable while bridge_buffers is being filled.
  size_storage->reserve(buffers.size());
  for (const StoreBuffer& buffer : buffers) {
    if (buffer.addresses.size() != buffer.sizes.size()) {
      return false;
    }
    std::vector<uint64_t> sizes;
    sizes.reserve(buffer.sizes.size());
    for (size_t size : buffer.sizes) {
      sizes.emplace_back(static_cast<uint64_t>(size));
    }
    size_storage->emplace_back(std::move(sizes));
  }
  bridge_buffers->reserve(buffers.size());
  for (size_t index = 0; index < buffers.size(); ++index) {
    bridge_buffers->push_back(BridgeBuffer{buffers[index].addresses.data(),
                                           (*size_storage)[index].data(),
                                           (*size_storage)[index].size()});
  }
  return true;
}

void YuanRongStoreBackend::encode_keys(const std::vector<std::string>& keys,
                                       std::vector<std::string>* encoded,
                                       std::vector<const char*>* raw) {
  encoded->reserve(keys.size());
  for (const std::string& key : keys) {
    encoded->emplace_back(encode_key(key));
  }
  raw->reserve(encoded->size());
  for (const std::string& key : *encoded) {
    raw->push_back(key.c_str());
  }
}

std::vector<uint8_t> YuanRongStoreBackend::batch_put(
    const std::vector<std::string>& keys,
    const std::vector<StoreBuffer>& buffers) {
  std::vector<uint8_t> statuses(keys.size(), /*value=*/0);
  if (keys.size() != buffers.size()) {
    LOG(ERROR) << "YuanRong store put requires one buffer per key, keys="
               << keys.size() << ", buffers=" << buffers.size();
    return statuses;
  }
  if (keys.empty()) {
    return statuses;
  }
  if (bridge_client_ == nullptr) {
    return statuses;
  }

  std::vector<std::vector<uint64_t>> size_storage;
  std::vector<BridgeBuffer> bridge_buffers;
  if (!marshal_buffers(buffers, &size_storage, &bridge_buffers)) {
    LOG(ERROR) << "YuanRong store put buffer has mismatched range arrays";
    return statuses;
  }

  std::vector<std::string> encoded_keys;
  std::vector<const char*> raw_keys;
  encode_keys(keys, &encoded_keys, &raw_keys);

  if (!bridge_mset_(bridge_client_,
                    raw_keys.data(),
                    bridge_buffers.data(),
                    raw_keys.size())) {
    const char* error = bridge_last_error_();
    LOG(ERROR) << "YuanRong store MSet failed: " << error;
    return statuses;
  }
  return std::vector<uint8_t>(keys.size(), /*value=*/1);
}

std::vector<uint8_t> YuanRongStoreBackend::batch_get(
    const std::vector<std::string>& keys,
    const std::vector<StoreBuffer>& buffers) {
  std::vector<uint8_t> statuses(keys.size(), /*value=*/0);
  if (keys.size() != buffers.size()) {
    LOG(ERROR) << "YuanRong store get requires one buffer per key, keys="
               << keys.size() << ", buffers=" << buffers.size();
    return statuses;
  }
  if (keys.empty()) {
    return statuses;
  }
  if (bridge_client_ == nullptr) {
    return statuses;
  }

  std::vector<std::vector<uint64_t>> size_storage;
  std::vector<BridgeBuffer> bridge_buffers;
  if (!marshal_buffers(buffers, &size_storage, &bridge_buffers)) {
    LOG(ERROR) << "YuanRong store get buffer has mismatched range arrays";
    return statuses;
  }

  std::vector<std::string> encoded_keys;
  std::vector<const char*> raw_keys;
  encode_keys(keys, &encoded_keys, &raw_keys);

  // One SDK Get for the whole prefetch. The bridge fills one status byte per
  // key: a partial hit is still a successful call, and a missing object only
  // clears its own byte.
  if (!bridge_mget_(bridge_client_,
                    raw_keys.data(),
                    bridge_buffers.data(),
                    statuses.data(),
                    raw_keys.size())) {
    const char* error = bridge_last_error_();
    LOG(ERROR) << "YuanRong store MGet failed for " << keys.size()
               << " keys: " << error;
    return std::vector<uint8_t>(keys.size(), /*value=*/0);
  }
  return statuses;
}

bool YuanRongStoreBackend::get(const std::string& key,
                               const StoreBuffer& buffer) {
  const std::vector<uint8_t> statuses = batch_get({key}, {buffer});
  return !statuses.empty() && statuses[0] != 0;
}

bool YuanRongStoreBackend::load_bridge() {
  if (bridge_handle_ != nullptr) {
    return true;
  }
  const std::string library = config_.bridge_path.empty()
                                  ? std::string(kDefaultBridgeLibrary)
                                  : config_.bridge_path;
  // The bridge hides its static SDK symbols at link time. DEEPBIND would
  // also bypass the global allocator for SDK free() calls, while the shared
  // C++ runtime can still allocate through that allocator.
  // DS owns process-wide registries and background state beyond KVClient.
  // Balance dlopen/dlclose handles while retaining the SDK until process exit.
  int32_t load_flags = RTLD_NOW | RTLD_LOCAL;
#ifdef RTLD_NODELETE
  load_flags |= RTLD_NODELETE;
#endif
  bridge_handle_ = dlopen(library.c_str(), load_flags);
  if (bridge_handle_ == nullptr) {
    LOG(ERROR) << "Failed to load the YuanRong store bridge " << library
               << " (set store_yuanrong_bridge_path to override): "
               << dlerror();
    return false;
  }

  auto load_symbol = [this](const char* name) -> void* {
    void* symbol = dlsym(bridge_handle_, name);
    if (symbol == nullptr) {
      LOG(ERROR) << "YuanRong store bridge is missing symbol " << name;
    }
    return symbol;
  };

  bridge_create_ =
      reinterpret_cast<BridgeCreateFn>(load_symbol("xllm_yr_store_create"));
  bridge_destroy_ =
      reinterpret_cast<BridgeDestroyFn>(load_symbol("xllm_yr_store_destroy"));
  bridge_mset_ =
      reinterpret_cast<BridgeMSetFn>(load_symbol("xllm_yr_store_mset"));
  bridge_mget_ =
      reinterpret_cast<BridgeMGetFn>(load_symbol("xllm_yr_store_mget"));
  bridge_last_error_ = reinterpret_cast<BridgeLastErrorFn>(
      load_symbol("xllm_yr_store_thread_error"));
  if (!bridge_create_ || !bridge_destroy_ || !bridge_mset_ || !bridge_mget_ ||
      !bridge_last_error_) {
    dlclose(bridge_handle_);
    bridge_handle_ = nullptr;
    return false;
  }
  return true;
}

bool YuanRongStoreBackend::initialize() {
  if (bridge_client_ != nullptr) {
    return true;
  }
  if (!load_bridge()) {
    return false;
  }
  if (!is_valid_yuanrong_port(config_.port)) {
    LOG(ERROR) << "Invalid store_yuanrong_port=" << config_.port
               << "; expected 0..65535 (0 lets the SDK choose its default).";
    return false;
  }
  bridge_client_ =
      bridge_create_(config_.host.c_str(), config_.port, config_.timeout_ms);
  if (bridge_client_ == nullptr) {
    LOG(ERROR) << "YuanRong store client create failed for " << config_.host
               << ":" << config_.port << ": " << bridge_last_error_();
    return false;
  }
  LOG(INFO) << "YuanRong store backend ready, worker=" << config_.host << ":"
            << config_.port;
  return true;
}

}  // namespace xllm
